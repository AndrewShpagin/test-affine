#include "jpeg_restart.h"
#include "udp_image_codec.h"
#include "flowx_protocol.h"
#include "flowx_raw_store.h"
#include "flowx_browser_assets.h"
#include <opencv2/imgproc.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>

namespace {
using Bytes = std::vector<unsigned char>;
using affinecodec::JpegRestartRegion;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void put(Bytes& b, unsigned pos, std::uint32_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) b[pos + i] = static_cast<unsigned char>(v >> (8 * i));
}
Bytes native(const JpegRestartRegion& r, std::uint32_t id = 100) {
    Bytes b(32, 0);
    put(b, 0, 0x31434641, 4); b[4] = 2; b[5] = 5; b[6] = 32;
    put(b, 8, id, 4); put(b, 12, id, 4);
    put(b, 16, 2 * r.layer_width, 2); put(b, 18, r.layer_height, 2);
    put(b, 20, r.layer_width, 2); put(b, 22, r.layer_height, 2);
    put(b, 24, r.x, 2); put(b, 26, r.y, 2); put(b, 28, r.width, 2); b[30] = r.profile;
    b.insert(b.end(), r.entropy.begin(), r.entropy.end());
    return b;
}
Bytes wire(const Bytes& b, std::uint32_t stream = 7) {
    Bytes out; std::string error;
    require(flowx::wrapCodecPacket(b, stream, 123456, out, &error), error.c_str());
    return out;
}
Bytes rgba(const cv::Mat& bgr) {
    cv::Mat result;
    cv::cvtColor(bgr, result, bgr.channels() == 1 ? cv::COLOR_GRAY2RGBA : cv::COLOR_BGR2RGBA);
    return Bytes(result.datastart, result.dataend);
}
struct Fixture {
    std::vector<JpegRestartRegion> regions;
    std::vector<Bytes> packets, wires;
    std::vector<cv::Mat> decoded;
    cv::Size size;
    int type;
};
Fixture fixture(unsigned channels, unsigned half_width = 256) {
    Fixture f; f.size = cv::Size(2 * half_width, 64); f.type = channels == 1 ? CV_8UC1 : CV_8UC3;
    cv::RNG random(34567);
    for (unsigned parity = 0; parity < 2; ++parity) {
        cv::Mat layer(f.size.height, half_width, f.type);
        random.fill(layer, cv::RNG::UNIFORM, 20, 230);
        std::vector<JpegRestartRegion> regions;
        unsigned interval = 0;
        require(affinecodec::encodeJpegRestartLayer(layer, parity, regions, &interval), "encode layer");
        require(interval && (half_width / 8) % interval == 0, "restart crosses an MCU row");
        require(regions.size() > 4, "fixture needs many regions");
        for (auto& r : regions) {
            cv::Mat decoded;
            require(affinecodec::decodeJpegRestartRegion(r, decoded), "independent JPEG decode");
            // The input has no black pixels: a lost predecessor must not produce black blocks.
            require(cv::mean(decoded)[0] > 50, "independent JPEG lost DC state");
            f.packets.push_back(native(r));
            f.wires.push_back(wire(f.packets.back()));
            require(f.wires.back().size() <= 1300, "datagram exceeds 1300 bytes");
            flowx::FlowXPacket restored;
            require(flowx::unwrapCodecPacket(f.wires.back(), restored), "unwrap region");
            require(restored.codec_packet == f.packets.back(), "region wire roundtrip");
            f.decoded.push_back(decoded);
            f.regions.push_back(std::move(r));
        }
    }
    return f;
}
// Build actual half-images and masks first, then independently interleave/fill.
cv::Mat expected(const Fixture& f, const std::vector<unsigned>& selected) {
    cv::Mat actual(f.size, f.type, cv::Scalar::all(0));
    cv::Mat mask(f.size, CV_8U, cv::Scalar(0));
    for (unsigned i : selected) {
        const auto& r = f.regions[i];
        for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < r.width; ++x) {
            const int dest_x = r.x + 2 * x, dest_y = r.y + y;
            f.decoded[i](cv::Rect(x, y, 1, 1)).copyTo(actual(cv::Rect(dest_x, dest_y, 1, 1)));
            mask.at<unsigned char>(dest_y, dest_x) = 1;
        }
    }
    cv::Mat result = actual.clone();
    for (int y = 0; y < f.size.height; ++y) for (int x = 0; x < f.size.width; ++x)
        if (!mask.at<unsigned char>(y, x) && mask.at<unsigned char>(y, x ^ 1))
            actual(cv::Rect(x ^ 1, y, 1, 1)).copyTo(result(cv::Rect(x, y, 1, 1)));
    return result;
}
cv::Mat render(affinecodec::Decoder& d) {
    cv::Mat image; d.render(image, {}, {}); require(!image.empty(), "render empty"); return image;
}
void same(const cv::Mat& a, const cv::Mat& b, const char* what) {
    require(a.size() == b.size() && a.type() == b.type() && cv::norm(a, b, cv::NORM_INF) == 0, what);
}
void lossTest(const Fixture& f) {
    std::vector<unsigned> all(f.packets.size());
    std::iota(all.begin(), all.end(), 0u);
    const auto complete = expected(f, all);
    affinecodec::PatchData p;
    p.frame_id = 102; p.keyframe_id = 100; p.original_size = f.size;
    p.grid_x = p.grid_y = 1; p.mesh.emplace_back(0, 0); p.affine = {1, 0, 0, 0, 1, 0};
    affinecodec::Decoder baseline;
    for (const auto& packet : f.packets) baseline.pushData(packet);
    cv::Mat complete_patch; baseline.render(complete_patch, {p}, {});
    for (unsigned seed = 0; seed < 12; ++seed) {
        auto order = all; std::mt19937 rng(seed); std::shuffle(order.begin(), order.end(), rng);
        const auto kept = order.size() * 65 / 100;
        affinecodec::Decoder decoder;
        unsigned events = 0; Bytes jpeg;
        for (unsigned k = 0; k < kept; ++k) {
            decoder.pushData(f.packets[order[k]]);
            events += decoder.updateKeyframe(jpeg);
            decoder.pushData(f.packets[order[k]]); // duplicate delivery
            require(!decoder.updateKeyframe(jpeg), "duplicate key event");
        }
        require(events == 1, "late region re-emits old frame");
        std::vector<unsigned> selected(order.begin(), order.begin() + kept);
        auto displayed = render(decoder), frozen = displayed.clone();
        same(displayed, expected(f, selected), "35% loss/fill differs");
        for (unsigned k = kept; k < order.size(); ++k) {
            decoder.pushData(f.packets[order[k]]);
            require(!decoder.updateKeyframe(jpeg), "late key event rewinds video");
        }
        same(displayed, frozen, "late key mutated displayed pixels");
        same(render(decoder), complete, "late data did not restore complete key");
        cv::Mat next; decoder.render(next, {p}, {});
        same(next, complete_patch, "next PATCH missed late key data");
    }
    // Both halves absent remain black; one half fills only its matching columns.
    affinecodec::Decoder only;
    only.pushData(f.packets.back());
    same(render(only), expected(f, {unsigned(f.packets.size() - 1)}), "single region placement/fill");
    // Newer frames replace masks. Old regions cannot resurrect a previous reference.
    auto fresh = native(f.regions.front(), 110);
    only.pushData(fresh); Bytes jpeg;
    require(only.updateKeyframe(jpeg) && only.keyframeId() == 110, "new key not accepted");
    auto before = render(only);
    only.pushData(f.packets.back());
    same(render(only), before, "stale key changed reference");
    auto bad = native(f.regions.back(), 111); bad[30] = 99;
    only.pushData(bad);
    require(only.keyframeId() == 110, "malformed key poisoned reference");
    flowx::FlowXPacket parsed;
    auto bad_wire = f.wires.front(); bad_wire.resize(1301);
    require(!flowx::unwrapCodecPacket(bad_wire, parsed), "oversize packet accepted");
    bad_wire = f.wires.front(); bad_wire[28] = 2;
    require(!flowx::unwrapCodecPacket(bad_wire, parsed), "unaligned coordinate accepted");
    bad_wire = f.wires.front(); bad_wire[35] = 1;
    require(!flowx::unwrapCodecPacket(bad_wire, parsed), "reserved field accepted");
    bad_wire = f.wires.front(); bad_wire[36] = 255; bad_wire[37] = 217;
    require(!flowx::unwrapCodecPacket(bad_wire, parsed), "in-band JPEG marker accepted");
    affinecodec::Decoder wrapped;
    wrapped.pushData(native(f.regions.front(), 0xfffffffe));
    wrapped.pushData(native(f.regions.back(), 1));
    require(wrapped.keyframeId() == 1, "frame-id wrap failed");
    wrapped.pushData(native(f.regions.front(), 0xfffffffe));
    require(wrapped.keyframeId() == 1, "wrapped stale frame accepted");
}
void rawStoreTest(const Fixture& f) {
    flowx::RawFrameStore store;
    const auto push = [&](const Bytes& b) {
        flowx::FlowXPacket p; require(flowx::unwrapCodecPacket(b, p), "store packet invalid");
        store.push(b, p.metadata);
    };
    push(f.wires[0]);
    auto first = store.latest(); require(first && first->datagrams.size() == 1, "region waits for next frame");
    push(f.wires[0]); require(store.latest()->sequence == first->sequence, "raw duplicate update");
    Bytes patch(56, 0);
    put(patch, 0, 0x31434641, 4); patch[4] = 2; patch[5] = 2; patch[6] = 56;
    put(patch, 8, 101, 4); put(patch, 12, 100, 4);
    put(patch, 16, f.size.width, 2); put(patch, 18, f.size.height, 2);
    patch[20] = patch[21] = 1; put(patch, 24, 0x3f800000, 4); put(patch, 40, 0x3f800000, 4);
    const auto patch_wire = wire(patch); push(patch_wire);
    const auto patch_seq = store.latest()->sequence;
    push(f.wires[1]); // active key is older than last displayed PATCH
    std::shared_ptr<const flowx::RawFrameBundle> bundle;
    require(store.waitForNext(patch_seq, bundle, std::chrono::milliseconds(0)), "late key not forwarded");
    require(bundle->datagrams == std::vector<Bytes>{f.wires[1]}, "late key delta wrong");
    push(f.wires[2]);
    require(store.waitForNext(first->sequence, bundle, std::chrono::milliseconds(0)), "slow browser catchup absent");
    require(bundle->datagrams.size() == 4 && bundle->datagrams.back() == patch_wire, "catchup omitted key/patch");
    require(store.waitForNext(0, bundle, std::chrono::milliseconds(0)) && bundle->datagrams.size() == 4,
            "new browser omitted key snapshot");
    auto old_snapshot = store.latestKeyframe();
    push(wire(native(f.regions.front(), 110)));
    push(f.wires[3]);
    require(store.latestKeyframe()->datagrams.size() == 1 && old_snapshot->datagrams.size() == 3,
            "stale key or mutable snapshot");
    push(wire(native(f.regions.front(), 1), 8));
    require(store.latestKeyframe()->stream_id == 8 && store.latestKeyframe()->frame_id == 1, "stream reset");
    store.close();
}
void encoderTest() {
    cv::Mat image(256, 512, CV_8UC3); cv::RNG(876).fill(image, cv::RNG::UNIFORM, 0, 256);
    affinecodec::Encoder encoder; encoder.setStripsKeyframes(true);
    encoder.pushImage(image, 18000, 10);
    Bytes b; unsigned count = 0; affinecodec::Decoder decoder;
    while (encoder.getNextChunk(b)) {
        require(b.size() > 32 && b[5] == 5, "STRIPS JPEG used fragile chunks");
        require(wire(b).size() <= 1300, "integrated encoder exceeded MTU");
        decoder.pushData(b); ++count;
    }
    require(count > 4 && decoder.originalSize() == image.size(), "integrated encode failed");
    require(render(decoder).size() == image.size(), "scaled reference render size");
}
void exportFixture(const Fixture& f, const std::string& path) {
    nlohmann::json j;
    j["headers"]["1"] = affinecodec::restartJpegHeader(1, 8);
    j["headers"]["2"] = affinecodec::restartJpegHeader(2, 8);
    j["width"] = f.size.width; j["height"] = f.size.height;
    for (unsigned i = 0; i < f.regions.size(); ++i) {
        Bytes jpeg; require(affinecodec::makeRestartJpeg(f.regions[i], jpeg), "fixture jpeg");
        j["regions"].push_back({{"wire", f.wires[i]}, {"jpeg", jpeg}, {"rgba", rgba(f.decoded[i])}});
    }
    std::vector<unsigned> all(f.packets.size()); std::iota(all.begin(), all.end(), 0u);
    j["complete"] = rgba(expected(f, all));
    std::ofstream out(path); out << j.dump(); require(out.good(), "write fixtures");
    std::ofstream js(path + ".js"); js << flowx::browserJs(); require(js.good(), "write browser JS");
    std::ofstream html(path + ".html"); html << flowx::browserHtml(); require(html.good(), "write browser HTML");
}
}
int main(int argc, char** argv) {
    try {
        cv::setNumThreads(2);
        auto color = fixture(3);
        if (argc == 3 && std::string(argv[1]) == "--export") { exportFixture(color, argv[2]); return 0; }
        lossTest(color); lossTest(fixture(1)); lossTest(fixture(3, 232));
        rawStoreTest(color); encoderTest();
        std::cout << "PASS: JPEG regions, 1300-byte MTU, 35% loss (36 shuffled trials), late/duplicate/stale packets, PATCH reference, HTTP catchup\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}

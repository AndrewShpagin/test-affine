#include "jpeg_restart.h"
#include "udp_image_codec.h"
#include "flowx_protocol.h"
#include "flowx_raw_store.h"
#include "flowx_browser_assets.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
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
    put(b, 24, r.x, 2); put(b, 26, r.y, 2); put(b, 28, r.width, 2); b[30] = r.profile; b[31] = r.layout;
    b.insert(b.end(), r.entropy.begin(), r.entropy.end());
    return b;
}
Bytes wire(const Bytes& b, std::uint32_t stream = 7) {
    Bytes out; std::string error;
    if (!flowx::wrapCodecPacket(b, stream, 123456, out, &error)) throw std::runtime_error(error);
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
    std::vector<std::uint32_t> tile_map;
};
Fixture fixture(unsigned channels, unsigned half_width = 256, bool shuffle = false, unsigned height = 64) {
    Fixture f; f.size = cv::Size(2 * half_width, height); f.type = channels == 1 ? CV_8UC1 : CV_8UC3;
    if (shuffle) f.tile_map = affinecodec::restartTilePermutation(half_width, f.size.height);
    cv::RNG random(34567);
    for (unsigned parity = 0; parity < 2; ++parity) {
        cv::Mat layer(f.size.height, half_width, f.type);
        random.fill(layer, cv::RNG::UNIFORM, 20, 230);
        std::vector<JpegRestartRegion> regions;
        unsigned interval = 0;
        const auto original = layer.clone();
        require(affinecodec::encodeJpegRestartLayer(layer, parity, regions, &interval, shuffle), "encode layer");
        require(cv::norm(layer, original, cv::NORM_INF) == 0, "shuffle mutated source/motion reference");
        const unsigned blocks = static_cast<unsigned>(layer.total() / 64);
        require(interval && regions.size() == (blocks + interval - 1) / interval, "restart coverage");
        require(regions.size() > 4, "fixture needs many regions");
        unsigned next = 0;
        for (auto& r : regions) {
            require((r.y / 8) * (half_width / 8) + r.x / 16 == next, "restart gap or overlap");
            require(r.width / 8 == std::min(interval, blocks - next), "wrong final segment length");
            next += r.width / 8;
            cv::Mat decoded;
            require(affinecodec::decodeJpegRestartRegion(r, decoded), "independent JPEG decode");
            // The input has no black pixels: a lost predecessor must not produce black blocks.
            require(cv::mean(decoded)[0] > 50, "independent JPEG lost DC state");
            f.packets.push_back(native(r));
            f.wires.push_back(wire(f.packets.back()));
            require(f.wires.back().size() <= flowx::kMaxUdpDatagramBytes, "datagram exceeds UDP hard limit");
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
            const unsigned columns = r.layer_width / 8;
            const unsigned encoded = (r.y / 8) * columns + r.x / 16 + x / 8;
            const unsigned target = f.tile_map.empty() ? encoded : f.tile_map[encoded];
            const int dest_x = (target % columns) * 16 + 2 * (x % 8) + (r.x & 1);
            const int dest_y = (target / columns) * 8 + y;
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
    for (bool fill : {false,true}) for (bool smooth : {false,true}) {
        affinecodec::Decoder concealed;
        concealed.setRestartFillOptions(fill,smooth);
        concealed.pushData(f.packets.front());
        const auto count = concealed.restartReceivedBlocks();
        auto first = render(concealed), frozen = first.clone();
        require(concealed.restartReceivedBlocks()==count && count==f.regions.front().width/8,
                "concealment changed receipt count");
        same(render(concealed),first,"repeated render accumulated blur");
        concealed.pushData(f.packets.front());
        require(concealed.restartReceivedBlocks()==count,"duplicate extended receipt count");
        for (const auto& packet : f.packets) concealed.pushData(packet);
        same(first,frozen,"late data changed published frame");
        same(render(concealed),complete,"late native concealment recovery failed");
        require(concealed.restartReceivedBlocks()==concealed.restartTotalBlocks(),"completion count failed");
        concealed.setRestartFillOptions(true,true);
        same(render(concealed),complete,"option change altered complete key");
    }
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
    auto bad_wire = f.wires.front(); bad_wire.resize(flowx::kMaxUdpDatagramBytes + 1);
    require(!flowx::unwrapCodecPacket(bad_wire, parsed), "oversize packet accepted");
    bad_wire = f.wires.front(); bad_wire[28] = 2;
    require(!flowx::unwrapCodecPacket(bad_wire, parsed), "unaligned coordinate accepted");
    bad_wire = f.wires.front(); put(bad_wire, 28, f.size.width, 2);
    require(!flowx::unwrapCodecPacket(bad_wire, parsed), "start beyond row accepted");
    bad_wire = f.wires.front();
    put(bad_wire, 28, f.size.width - 16, 2); put(bad_wire, 30, f.size.height - 8, 2);
    put(bad_wire, 32, 16, 2);
    require(!flowx::unwrapCodecPacket(bad_wire, parsed), "restart past final block accepted");
    bad_wire = f.wires.front(); bad_wire[35] = 2;
    require(!flowx::unwrapCodecPacket(bad_wire, parsed), "unknown layout accepted");
    auto mixed = fresh; mixed[31] ^= 1;
    only.pushData(mixed);
    same(render(only), before, "mixed layout changed same-key reference");
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
void encoderTest(bool shuffle) {
    cv::Mat image(256, 512, CV_8UC3); cv::RNG(876).fill(image, cv::RNG::UNIFORM, 0, 256);
    affinecodec::Encoder encoder; encoder.setStripsKeyframes(true);
    encoder.setJpegTileShuffle(shuffle);
    encoder.pushImage(image, 18000, 10);
    Bytes b; unsigned count = 0; affinecodec::Decoder decoder;
    while (encoder.getNextChunk(b)) {
        require(b.size() > 32 && b[5] == 5, "STRIPS JPEG used fragile chunks");
        require(b[31] == unsigned(shuffle), "encoder omitted shuffle mode");
        require(wire(b).size() <= flowx::kMaxUdpDatagramBytes, "integrated encoder exceeded UDP limit");
        decoder.pushData(b); ++count;
    }
    require(count > 4 && decoder.originalSize() == image.size(), "integrated encode failed");
    require(encoder.lastTiming().jpeg_encode_calls == 2, "keyframe compressed more than once per half");
    require(render(decoder).size() == image.size(), "scaled reference render size");
    encoder.setJpegTileShuffle(!shuffle);
    encoder.pushImage(image, 18000, 1);
    while (encoder.getNextChunk(b)) {
        require(b[5] == 5 && b[31] == unsigned(!shuffle), "runtime shuffle switch not applied to next key");
        decoder.pushData(b);
    }
    require(render(decoder).size() == image.size(), "decoder failed a layout transition");
}
void shuffleTest(unsigned channels) {
    const auto plain = fixture(channels), shuffled = fixture(channels, 256, true);
    std::vector<unsigned> a(plain.packets.size()), b(shuffled.packets.size());
    std::iota(a.begin(), a.end(), 0u); std::iota(b.begin(), b.end(), 0u);
    same(expected(plain, a), expected(shuffled, b), "shuffle changed no-loss JPEG pixels");
    affinecodec::Decoder decoder;
    for (const auto& packet : shuffled.packets) decoder.pushData(packet);
    same(render(decoder), expected(plain, a), "decoder did not undo permutation");
    // A lost encoded row must spread across several spatial rows.
    const auto& r = shuffled.regions.front();
    std::vector<unsigned> rows;
    for (unsigned i = 0; i < r.width / 8; ++i) rows.push_back(shuffled.tile_map[i] / (r.layer_width / 8));
    std::sort(rows.begin(), rows.end());
    require(rows.front() != rows.back(), "packet stayed in a single spatial row");
}
void permutationTest() {
    require(affinecodec::restartTilePermutation(32, 16) ==
            std::vector<std::uint32_t>({0, 6, 5, 7, 2, 1, 3, 4}), "shuffle v1 wire vector changed");
    for (const auto size : {cv::Size(8, 8), cv::Size(8, 248), cv::Size(56, 24),
                            cv::Size(232, 64), cv::Size(32760, 8)}) {
        auto map = affinecodec::restartTilePermutation(size.width, size.height);
        require(map.size() == unsigned(size.area() / 64), "shuffle size wrong");
        std::sort(map.begin(), map.end());
        for (unsigned i = 0; i < map.size(); ++i) require(map[i] == i, "shuffle is not a bijection");
    }
    require(affinecodec::restartTilePermutation(0, 8).empty(), "invalid shuffle dimensions");
}
void packingTest() {
    // Narrow, low-entropy images must be able to grow beyond both the row
    // width and the initial 56-MCU estimate. Decode against independent tiles
    // to check DC state, row wrapping, and the short final segment.
    for (unsigned channels : {1u, 3u}) for (bool shuffle : {false, true}) {
        cv::Mat layer(512, 40, channels == 1 ? CV_8UC1 : CV_8UC3);
        for (int y = 0; y < layer.rows; ++y) for (int x = 0; x < layer.cols; ++x)
            for (unsigned c = 0; c < channels; ++c)
                layer.ptr(y)[x * channels + c] = static_cast<unsigned char>(40 + (x * 3 + y * 2 + c * 30) % 170);
        const unsigned columns = layer.cols / 8, blocks = static_cast<unsigned>(layer.total() / 64);
        const auto map = affinecodec::restartTilePermutation(layer.cols, layer.rows);
        std::vector<JpegRestartRegion> regions;
        unsigned interval = 0, calls = 0;
        require(affinecodec::encodeJpegRestartLayer(layer, 1, regions, &interval, shuffle, 0, &calls), "narrow encode");
        require(calls == 1, "first narrow encode retried");
        double cost = 0;
        for (const auto& r : regions) cost = std::max(cost, r.entropy.size() / double(r.width / 8));
        require(affinecodec::encodeJpegRestartLayer(layer, 1, regions, &interval, shuffle, cost, &calls), "next narrow encode");
        require(calls == 2, "feedback encode retried");
        require(interval > columns, "narrow image still capped by row width");
        std::size_t bytes = 0;
        unsigned seen = 0;
        for (const auto& r : regions) {
            require(affinecodec::validRestartGeometry(r, {2 * layer.cols, layer.rows}), "cross-row geometry");
            bytes += wire(native(r)).size();
            cv::Mat strip;
            require(affinecodec::decodeJpegRestartRegion(r, strip), "cross-row standalone JPEG");
            for (unsigned b = 0; b < r.width / 8; ++b) {
                const unsigned spatial = shuffle ? map[seen + b] : seen + b;
                const auto tile = layer(cv::Rect((spatial % columns) * 8, (spatial / columns) * 8, 8, 8));
                std::vector<JpegRestartRegion> single;
                cv::Mat reference;
                require(affinecodec::encodeJpegRestartLayer(tile, 1, single) && single.size() == 1 &&
                        affinecodec::decodeJpegRestartRegion(single[0], reference), "independent tile reference");
                same(strip(cv::Rect(b * 8, 0, 8, 8)), reference, "cross-row DC or pixel mismatch");
            }
            seen += r.width / 8;
        }
        require(seen == blocks, "narrow coverage lost final blocks");
        require(regions.back().width / 8 == blocks - (regions.size() - 1) * interval, "tail MCU count");
        require(double(bytes) / regions.size() > 800, "narrow JPEG datagrams remain underfilled");
        std::cout << "  narrow channels=" << channels << " shuffle=" << shuffle << " interval=" << interval
                  << " mean=" << double(bytes) / regions.size() << " B packets=" << regions.size() << '\n';
    }
    cv::Mat flat(512, 40, CV_8UC1, cv::Scalar(100));
    std::vector<JpegRestartRegion> regions;
    unsigned interval = 0;
    unsigned calls = 0;
    require(affinecodec::encodeJpegRestartLayer(flat, 0, regions, &interval, false, 0, &calls) && interval == 56,
            "first flat frame changed initial estimate");
    double cost = 0;
    for (const auto& r : regions) cost = std::max(cost, r.entropy.size() / double(r.width / 8));
    require(affinecodec::encodeJpegRestartLayer(flat, 0, regions, &interval, false, cost, &calls) &&
            calls == 2 && interval > 56 && regions.size() == 1, "next flat frame did not use feedback");
}
void singlePassTest() {
    affinecodec::Encoder encoder;
    encoder.setStripsKeyframes(true);
    cv::Mat flat(512, 512, CV_8UC3, cv::Scalar::all(128)), noise(flat.size(), flat.type());
    cv::RNG(91827).fill(noise, cv::RNG::UNIFORM, 0, 256);
    auto encodeFrame = [&](cv::Mat& source, int budget) {
        encoder.pushImage(source, budget, 1);
        require(encoder.lastTiming().jpeg_encode_calls == 2, "scene/budget change triggered JPEG retry");
        std::size_t maximum = 0, packets = 0;
        Bytes b;
        while (encoder.getNextChunk(b)) {
            maximum = std::max(maximum, wire(b).size());
            ++packets;
        }
        require(packets || encoder.lastTiming().jpeg_unsendable_chunks, "one-pass encoder emitted nothing");
        return maximum;
    };
    encodeFrame(flat, 1000000);
    const auto changed_max = encodeFrame(noise, 1000000);
    require(changed_max > 1300 || encoder.lastTiming().jpeg_unsendable_chunks, "scene cut never exceeded target");
    const auto hard_drops = encoder.lastTiming().jpeg_unsendable_chunks;
    const auto recovered_max = encodeFrame(noise, 1000000);
    require(!encoder.lastTiming().jpeg_unsendable_chunks && recovered_max < changed_max,
            "next keyframe failed to adapt after scene cut");
    const auto old_size = encoder.lastTiming().jpeg_size;
    encodeFrame(noise, 4000);
    require(encoder.lastTiming().jpeg_size.area() < old_size.area(), "budget change did not resize next key");
    std::cout << "  scene cut max=" << changed_max << " B hard-drops=" << hard_drops
              << " next-max=" << recovered_max << " B, exactly two JPEG calls/key\n";

    // Deliberately stale, extremely optimistic feedback can exceed even UDP's
    // absolute limit. It still consumes just one compression and is rejected
    // by wire validation without truncating an entropy stream.
    std::vector<JpegRestartRegion> large;
    unsigned calls = 0;
    require(affinecodec::encodeJpegRestartLayer(noise, 0, large, nullptr, false, .1, &calls) && calls == 1,
            "absolute oversize retried compression");
    require(large.front().entropy.size() > affinecodec::kRestartMaxEntropyBytes, "hard-limit fixture too small");
    Bytes datagram;
    require(!flowx::wrapCodecPacket(native(large.front()), 7, 0, datagram), "unsendable segment accepted");
}
void exportFixture(const Fixture& f, const std::string& path) {
    nlohmann::json j;
    j["headers"]["1"] = affinecodec::restartJpegHeader(1, 8);
    j["headers"]["2"] = affinecodec::restartJpegHeader(2, 8);
    j["width"] = f.size.width; j["height"] = f.size.height;
    j["tile_map"] = f.tile_map;
    for (unsigned i = 0; i < f.regions.size(); ++i) {
        Bytes jpeg; require(affinecodec::makeRestartJpeg(f.regions[i], jpeg), "fixture jpeg");
        j["regions"].push_back({{"wire", f.wires[i]}, {"jpeg", jpeg}, {"rgba", rgba(f.decoded[i])}});
    }
    std::vector<unsigned> all(f.packets.size()); std::iota(all.begin(), all.end(), 0u);
    j["complete"] = rgba(expected(f, all));
    // Native outputs before HTTP re-encoding, for browser and receiver checks.
    std::vector<unsigned> scattered;
    for (unsigned i=0;i<all.size();++i) if ((i*37+11)%100<65) scattered.push_back(i);
    for (const auto& selected : {std::vector<unsigned>{0}, scattered}) {
        for (const std::string mode : {"raw","nearest","smooth"}) {
            affinecodec::Decoder decoder;
            decoder.setRestartFillOptions(mode!="raw",mode=="smooth");
            for (auto i : selected) decoder.pushData(f.packets[i]);
            auto image = render(decoder); Bytes jpeg;
            require(cv::imencode(".jpg",image,jpeg,{cv::IMWRITE_JPEG_QUALITY,85}),"encode fill fixture");
            j["concealment"].push_back({{"mode",mode},{"indices",selected},{"rgba",rgba(image)},{"jpeg",jpeg}});
        }
    }
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
        if (argc == 3 && std::string(argv[1]) == "--export-shuffled") { exportFixture(fixture(3, 256, true), argv[2]); return 0; }
        if (argc == 3 && std::string(argv[1]) == "--export-narrow") { exportFixture(fixture(3, 40, false, 512), argv[2]); return 0; }
        lossTest(color); lossTest(fixture(1)); lossTest(fixture(3, 232));
        lossTest(fixture(3, 256, true)); lossTest(fixture(1, 256, true)); lossTest(fixture(3, 232, true));
        rawStoreTest(color); rawStoreTest(fixture(3, 256, true));
        lossTest(fixture(3, 40, false, 512)); lossTest(fixture(3, 40, true, 512));
        encoderTest(false); encoderTest(true); permutationTest(); shuffleTest(1); shuffleTest(3); packingTest(); singlePassTest();
        std::cout << "PASS: one-pass JPEG, next-key feedback, soft target overshoot, cross-row regions, 35% loss (96 trials), tile shuffle/no-loss equivalence, late/duplicate/stale packets, PATCH reference, HTTP catchup\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}

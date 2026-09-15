#include "udp_image_codec.h"
#include "jpeg_restart.h"

#include <chrono>
#include <cstring>

namespace affinecodec {
namespace {
bool newer(std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a - b) > 0; }
std::uint16_t u16(const std::vector<u_char>& b, unsigned p) {
    return static_cast<std::uint16_t>(b[p] | (static_cast<unsigned>(b[p + 1]) << 8));
}
}

void Decoder::acceptRestartRegion(const std::vector<u_char>& data,
                                  std::uint32_t frame_id, cv::Size original) {
    if (data.size() <= kRestartHeaderBytes) return;
    JpegRestartRegion r;
    r.layer_width = u16(data, 20); r.layer_height = u16(data, 22);
    r.x = u16(data, 24); r.y = u16(data, 26); r.width = u16(data, 28);
    r.profile = data[30]; r.layout = data[31];
    if (!validRestartGeometry(r, original)) return;
    r.entropy.assign(data.begin() + kRestartHeaderBytes, data.end());
    if (!validRestartEntropy(r.entropy)) return;
    if (have_keyframe_ && frame_id != keyframe_id_ && !newer(frame_id, keyframe_id_)) return;
    if (have_keyframe_ && frame_id == keyframe_id_ &&
        (!pending_restart_.active || pending_restart_.frame_id != frame_id)) return;
    if (pending_keyframe_.active && !newer(frame_id, pending_keyframe_.frame_id)) return;
    if (pending_mosaic_.active && !newer(frame_id, pending_mosaic_.frame_id)) return;
    auto& a = pending_restart_;
    const bool new_keyframe = !a.active || a.frame_id != frame_id;
    if (a.active && new_keyframe && !newer(frame_id, a.frame_id)) return;
    if (!new_keyframe && (a.original_size != original || a.profile != r.profile || a.layout != r.layout ||
        a.layer_size != cv::Size(r.layer_width, r.layer_height))) return;
    const unsigned columns = r.layer_width / 8;
    const unsigned first = (r.y / 8) * columns + r.x / 16;
    const unsigned count = r.width / 8;
    const unsigned parity = r.x & 1;
    const auto spatial = [&](unsigned index) { return a.tile_map.empty() ? index : a.tile_map[index]; };
    if (!new_keyframe) {
        bool missing = false;
        for (unsigned i = 0; i < count; ++i) missing |= !a.received_blocks[2 * spatial(first + i) + parity];
        if (!missing) return;
    }
    cv::Mat decoded;
    const auto start = std::chrono::steady_clock::now();
    if (!decodeJpegRestartRegion(r, decoded)) return;
    const double decode_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    // Validate and decode before replacing a useful reference.
    if (new_keyframe) {
        a = RestartAssembly{};
        a.active = true; a.frame_id = frame_id; a.original_size = original;
        a.layer_size = cv::Size(r.layer_width, r.layer_height); a.profile = r.profile;
        a.layout = r.layout;
        if (a.layout == kRestartTileShuffle) a.tile_map = restartTilePermutation(r.layer_width, r.layer_height);
        a.assembled = cv::Mat(r.layer_height, 2 * r.layer_width, decoded.type(), cv::Scalar::all(0));
        a.received_blocks.assign(2u * columns * (r.layer_height / 8), 0);
        pending_keyframe_ = KeyframeAssembly{};
        pending_mosaic_ = MosaicAssembly{};
        pending_patch_queue_.clear(); patch_queue_.clear();
        original_size_ = original; keyframe_id_ = frame_id; have_keyframe_ = true;
        current_jpeg_.clear(); previous_render_.release();
        keyframe_changed_ = true;
        last_keyframe_image_decode_ms_ = 0.0;
    }
    const std::size_t pixel_bytes = decoded.elemSize();
    for (unsigned block = 0; block < count; ++block) {
        const unsigned target_block = spatial(first + block);
        const unsigned own = 2 * target_block + parity;
        if (a.received_blocks[own]) continue;
        const bool fill_other = !a.received_blocks[own ^ 1];
        for (unsigned y = 0; y < 8; ++y) {
            for (unsigned x = 0; x < 8; ++x) {
                const unsigned sample = block * 8 + x;
                const unsigned target = (target_block % columns) * 16 + 2 * x + parity;
                const auto* source = decoded.ptr(y) + sample * pixel_bytes;
                auto* row = a.assembled.ptr((target_block / columns) * 8 + y);
                std::memcpy(row + target * pixel_bytes, source, pixel_bytes);
                if (fill_other) std::memcpy(row + (target ^ 1u) * pixel_bytes, source, pixel_bytes);
            }
        }
        a.received_blocks[own] = 1;
    }
    decoded_keyframe_ = a.assembled;
    last_keyframe_image_decode_ms_ += decode_ms;
    // Same-key updates affect subsequent PATCH rendering only. Do not re-emit
    // an old displayed frame or invalidate its border-reuse buffer.
}

} // namespace affinecodec

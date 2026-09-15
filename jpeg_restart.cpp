#include "jpeg_restart.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <jpeglib.h>

namespace affinecodec {
namespace {

struct CompressState {
    jpeg_compress_struct jpeg{};
    jpeg_error_mgr error{};
    std::jmp_buf jump;
    unsigned char* memory = nullptr;
    unsigned long size = 0;
};

void jpegFailure(j_common_ptr jpeg) {
    auto* state = static_cast<CompressState*>(jpeg->client_data);
    std::longjmp(state->jump, 1);
}

bool encode(const cv::Mat& source, unsigned interval, std::vector<unsigned char>& bytes) {
    cv::Mat input;
    if (source.type() == CV_8UC1) input = source;
    else if (source.type() == CV_8UC3) cv::cvtColor(source, input, cv::COLOR_BGR2RGB);
    else return false;
    // Heap state remains defined after longjmp. No C++ objects are constructed
    // across calls that may invoke the libjpeg error callback.
    auto state = std::make_unique<CompressState>();
    auto& c = state->jpeg;
    c.err = jpeg_std_error(&state->error);
    state->error.error_exit = jpegFailure;
    c.client_data = state.get();
    if (setjmp(state->jump)) {
        jpeg_destroy_compress(&c);
        std::free(state->memory);
        return false;
    }
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &state->memory, &state->size);
    c.image_width = input.cols;
    c.image_height = input.rows;
    c.input_components = input.channels();
    c.in_color_space = input.channels() == 1 ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 85, TRUE);
    for (int i = 0; i < c.num_components; ++i) {
        c.comp_info[i].h_samp_factor = 1;
        c.comp_info[i].v_samp_factor = 1;
    }
    c.optimize_coding = FALSE;
    c.arith_code = FALSE;
    c.restart_interval = interval;
    c.dct_method = JDCT_ISLOW;
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = const_cast<JSAMPROW>(input.ptr(c.next_scanline));
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    bytes.assign(state->memory, state->memory + state->size);
    jpeg_destroy_compress(&c);
    std::free(state->memory);
    return true;
}

unsigned big16(const std::vector<unsigned char>& b, std::size_t p) {
    return (static_cast<unsigned>(b[p]) << 8) | b[p + 1];
}

bool scanOffset(const std::vector<unsigned char>& b, std::size_t& offset) {
    if (b.size() < 4 || b[0] != 0xff || b[1] != 0xd8) return false;
    for (std::size_t p = 2; p + 4 <= b.size();) {
        if (b[p] != 0xff) return false;
        const unsigned length = big16(b, p + 2);
        if (length < 2 || p + 2 + length > b.size()) return false;
        const auto marker = b[p + 1];
        p += 2 + length;
        if (marker == 0xda) { offset = p; return true; }
    }
    return false;
}

bool split(const std::vector<unsigned char>& b,
           std::vector<std::vector<unsigned char>>& segments) {
    std::size_t begin = 0;
    if (!scanOffset(b, begin)) return false;
    unsigned restart = 0;
    segments.clear();
    for (std::size_t p = begin; p + 1 < b.size(); ++p) {
        if (b[p] != 0xff) continue;
        const auto marker = b[++p];
        if (marker == 0) continue;
        if (marker != 0xd9 && marker != 0xd0 + (restart & 7)) return false;
        if (p - 1 == begin) return false;
        segments.emplace_back(b.begin() + begin, b.begin() + p - 1);
        if (marker == 0xd9) return p + 1 == b.size();
        ++restart;
        begin = p + 1;
    }
    return false;
}

unsigned rowDivisor(unsigned columns, unsigned limit) {
    unsigned n = std::min(columns, std::max(1u, limit));
    while (columns % n) --n;
    return n;
}

std::vector<unsigned char> fixedHeader(unsigned channels) {
    cv::Mat blank(8, 8, channels == 1 ? CV_8UC1 : CV_8UC3, cv::Scalar::all(0));
    std::vector<unsigned char> jpeg;
    std::size_t offset = 0;
    if (!encode(blank, 0, jpeg) || !scanOffset(jpeg, offset)) return {};
    jpeg.resize(offset);
    return jpeg;
}

} // namespace

bool validRestartGeometry(const JpegRestartRegion& r, cv::Size original) {
    return original.width >= 16 && original.height >= 8 && !(original.width & 1) &&
        static_cast<std::uint64_t>(original.width) * original.height <= kRestartMaxImagePixels &&
        r.layer_width >= 8 && r.layer_height >= 8 &&
        !(r.layer_width % 8) && !(r.layer_height % 8) &&
        2u * r.layer_width <= static_cast<unsigned>(original.width) &&
        r.layer_height <= original.height && r.width >= 8 && !(r.width % 8) &&
        !((r.x / 2) % 8) && !(r.y % 8) &&
        r.x / 2u + r.width <= r.layer_width && r.y + 8u <= r.layer_height &&
        (r.profile == kRestartGray85 || r.profile == kRestartColor44485) &&
        r.layout <= kRestartTileShuffle;
}

bool validRestartEntropy(const std::vector<unsigned char>& bytes) {
    if (bytes.empty() || bytes.size() > kRestartMaxEntropyBytes) return false;
    for (std::size_t p = 0; p < bytes.size(); ++p) {
        if (bytes[p] == 0xff && (++p == bytes.size() || bytes[p] != 0)) return false;
    }
    return true;
}

std::vector<std::uint32_t> restartTilePermutation(unsigned width, unsigned height) {
    if (width < 8 || height < 8 || width > 32760 || height > 65528 ||
        width % 8 || height % 8 || std::uint64_t(width) * height * 2 > kRestartMaxImagePixels)
        return {};
    std::vector<std::uint32_t> map((width / 8) * (height / 8));
    std::iota(map.begin(), map.end(), 0u);
    // Wire-defined Fisher-Yates with xorshift32; do not use std::shuffle,
    // whose exact permutation is not portable between C++ libraries and JS.
    std::uint32_t state = 0x46584a31u ^ width ^ (height << 16);
    for (std::size_t i = map.size() - 1; i > 0; --i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        std::swap(map[i], map[state % (i + 1)]);
    }
    return map;
}

bool encodeJpegRestartLayer(const cv::Mat& image, unsigned parity,
                            std::vector<JpegRestartRegion>& regions, unsigned* used_interval,
                            bool tile_shuffle) {
    regions.clear();
    if (image.empty() || parity > 1 || image.cols % 8 || image.rows % 8 ||
        image.cols > 32760 || image.rows > 65528 ||
        image.total() * 2 > kRestartMaxImagePixels ||
        (image.type() != CV_8UC1 && image.type() != CV_8UC3)) return false;
    const unsigned columns = image.cols / 8;
    cv::Mat input = image;
    if (tile_shuffle) {
        const auto map = restartTilePermutation(image.cols, image.rows);
        // Allocate separately: the original pixels also drive motion estimation.
        input = cv::Mat(image.size(), image.type());
        for (unsigned i = 0; i < map.size(); ++i) {
            const unsigned source = map[i];
            image(cv::Rect((source % columns) * 8, (source / columns) * 8, 8, 8))
                .copyTo(input(cv::Rect((i % columns) * 8, (i / columns) * 8, 8, 8)));
        }
    }
    unsigned interval = rowDivisor(columns,
        static_cast<unsigned>(kRestartMaxEntropyBytes / (64.0 * 0.35)));
    std::vector<unsigned char> jpeg;
    std::vector<std::vector<unsigned char>> segments;
    for (;;) {
        if (!encode(input, interval, jpeg) || !split(jpeg, segments)) return false;
        if (segments.size() != image.total() / (64u * interval)) return false;
        std::size_t largest = 0;
        for (const auto& segment : segments) largest = std::max(largest, segment.size());
        if (largest <= kRestartMaxEntropyBytes) break;
        if (interval == 1) return false;
        const unsigned limit = std::min(interval - 1,
            static_cast<unsigned>(interval * kRestartMaxEntropyBytes / largest));
        interval = rowDivisor(columns, limit);
    }
    std::vector<JpegRestartRegion> result;
    result.reserve(segments.size());
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const unsigned block = static_cast<unsigned>(i) * interval;
        JpegRestartRegion r;
        r.layer_width = static_cast<std::uint16_t>(image.cols);
        r.layer_height = static_cast<std::uint16_t>(image.rows);
        r.x = static_cast<std::uint16_t>(2 * (block % columns) * 8 + parity);
        r.y = static_cast<std::uint16_t>((block / columns) * 8);
        r.width = static_cast<std::uint16_t>(interval * 8);
        r.profile = image.channels() == 1 ? kRestartGray85 : kRestartColor44485;
        r.layout = tile_shuffle ? kRestartTileShuffle : 0;
        r.entropy = std::move(segments[i]);
        if (!validRestartEntropy(r.entropy)) return false;
        result.push_back(std::move(r));
    }
    if (used_interval) *used_interval = interval;
    regions.swap(result);
    return true;
}

std::vector<unsigned char> restartJpegHeader(std::uint8_t profile, unsigned width) {
    if ((profile != kRestartGray85 && profile != kRestartColor44485) ||
        width < 8 || width > 65528 || width % 8) return {};
    static const auto gray = fixedHeader(1);
    static const auto color = fixedHeader(3);
    auto header = profile == kRestartGray85 ? gray : color;
    for (std::size_t p = 2; p + 4 <= header.size();) {
        const unsigned length = big16(header, p + 2);
        if (length < 2 || p + 2 + length > header.size()) return {};
        if (header[p + 1] == 0xc0) {
            header[p + 5] = 0; header[p + 6] = 8;
            header[p + 7] = static_cast<unsigned char>(width >> 8);
            header[p + 8] = static_cast<unsigned char>(width);
            return header;
        }
        p += length + 2;
    }
    return {};
}

bool makeRestartJpeg(const JpegRestartRegion& r, std::vector<unsigned char>& jpeg) {
    jpeg.clear();
    if (!validRestartEntropy(r.entropy)) return false;
    const auto header = restartJpegHeader(r.profile, r.width);
    if (header.empty()) return false;
    jpeg.resize(header.size() + r.entropy.size() + 2);
    std::copy(header.begin(), header.end(), jpeg.begin());
    std::copy(r.entropy.begin(), r.entropy.end(), jpeg.begin() + header.size());
    jpeg[jpeg.size() - 2] = 0xff; jpeg.back() = 0xd9;
    return true;
}

bool decodeJpegRestartRegion(const JpegRestartRegion& region, cv::Mat& image) {
    image.release();
    std::vector<unsigned char> jpeg;
    if (!makeRestartJpeg(region, jpeg)) return false;
    try { image = cv::imdecode(jpeg, cv::IMREAD_UNCHANGED); }
    catch (const cv::Exception&) { return false; }
    return !image.empty() && image.rows == 8 && image.cols == region.width &&
        image.type() == (region.profile == kRestartGray85 ? CV_8UC1 : CV_8UC3);
}

} // namespace affinecodec

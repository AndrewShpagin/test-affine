#pragma once

#include <opencv2/core.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace affinecodec {

// Fixed profiles: IJG quality 85, standard Huffman tables, baseline sequential.
// Color uses YCbCr 4:4:4 so every MCU occupies exactly 8x8 pixels.
constexpr std::uint8_t kRestartGray85 = 1;
constexpr std::uint8_t kRestartColor44485 = 2;
constexpr std::uint8_t kRestartTileShuffle = 1; // layout byte: shared 16x8 permutation v1
constexpr std::uint8_t kRestartPacketType = 5; // internal AFC1
constexpr std::size_t kRestartHeaderBytes = 32;
constexpr std::size_t kRestartWireHeaderBytes = 36;
constexpr std::size_t kRestartMaxEntropyBytes = 1300 - kRestartWireHeaderBytes;
constexpr std::uint64_t kRestartMaxImagePixels = 16u * 1024u * 1024u;

struct JpegRestartRegion {
    std::uint16_t layer_width = 0;
    std::uint16_t layer_height = 0;
    // Coordinates in the encoded (possibly shuffled/downscaled) keyframe raster.
    // x parity identifies the half; sample i goes at column x + 2*i.
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0; // decoded sample count, height is always 8
    std::uint8_t profile = 0;
    std::uint8_t layout = 0; // 0: spatial order; 1: shuffled 16x8 tiles
    std::vector<unsigned char> entropy;
};

bool validRestartGeometry(const JpegRestartRegion& region, cv::Size original);
bool validRestartEntropy(const std::vector<unsigned char>& entropy);
// Maps encoded half-image block index to its original spatial block index.
// Identical for both parities; independent of packet arrival order and frame ID.
std::vector<std::uint32_t> restartTilePermutation(unsigned layer_width, unsigned layer_height);
bool encodeJpegRestartLayer(const cv::Mat& image, unsigned parity,
                            std::vector<JpegRestartRegion>& regions,
                            unsigned* restart_interval = nullptr, bool tile_shuffle = false);
// Builds a normal standalone JPEG locally; no header/table packet is needed.
std::vector<unsigned char> restartJpegHeader(std::uint8_t profile, unsigned width);
bool makeRestartJpeg(const JpegRestartRegion& region, std::vector<unsigned char>& jpeg);
bool decodeJpegRestartRegion(const JpegRestartRegion& region, cv::Mat& image);

} // namespace affinecodec

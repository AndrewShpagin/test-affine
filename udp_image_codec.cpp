#include "udp_image_codec.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace affinecodec {
namespace {

using ProfileClock = std::chrono::steady_clock;

double profileMs(const ProfileClock::time_point& start) {
    return std::chrono::duration<double, std::milli>(ProfileClock::now() - start).count();
}

constexpr bool kShowLKImage = true;
constexpr bool kMosaicFillMissing = true;

constexpr std::uint32_t kMagic = 0x31434641u;
constexpr std::uint8_t kVersion = 2;
constexpr std::uint8_t kPacketKeyframeChunk = 1;
constexpr std::uint8_t kPacketPatch = 2;
constexpr std::uint8_t kPacketMosaicKeyframeChunk = 3;
constexpr std::uint8_t kPacketMosaicKeyframeEnd = 4;
constexpr std::uint8_t kStripsLayerCount = 2;
constexpr std::uint8_t kMosaicLayerCount = 3;
constexpr std::uint16_t kPatchFlagHomography = 1u << 0;
constexpr std::uint16_t kPatchKnownFlags = kPatchFlagHomography;

constexpr int kFeatureGridX = 8;
constexpr int kFeatureGridY = 8;
constexpr int kFeaturesPerCell = 4;
constexpr int kFeatureCandidateMultiplier = 4;
constexpr int kFeatureDetectorMaxSide = 256;
constexpr int kJpegQuality = 85;
constexpr int kJpeg2000MinCompressionX1000 = 1;
constexpr int kJpeg2000MaxCompressionX1000 = 1000;
constexpr double kJpegTargetFill = 0.95;
constexpr double kJpegSizeTolerance = 0.20;
constexpr int kJpegMaxEncodePasses = 3;
constexpr double kJpegModelAlpha = 1.0;
constexpr double kJpegInitialGrayBytesPerPixel = 0.16;
constexpr double kJpegInitialColorBytesPerPixel = 0.22;
constexpr double kJpegMinBytesPerPixel = 0.01;
constexpr int kMeshGridX = 6;
constexpr int kMeshGridY = 6;
const cv::Size kLkWindow(13, 13);
constexpr int kLkMaxLevel = 3;
constexpr float kLkForwardErrorMax = 35.0f;
constexpr float kLkBackwardErrorMax = 1.5f;
constexpr double kAffineHuberPixels = 3.0;
constexpr int kAffineIterations = 5;
constexpr double kHomographyHuberPixels = 3.0;
constexpr double kHomographyMaxDenominatorVariation = 0.08;
constexpr int kHomographyIterations = 5;
constexpr double kTargetBrightness = 128.0;
constexpr double kTargetLkStdDev = 64.0;
constexpr std::size_t kCommonHeaderBytes = 20;
constexpr std::size_t kKeyframeChunkHeaderBytes = 40;
constexpr std::size_t kKeyframeChunkPayloadBytes = kMaxUdpPacketBytes - kKeyframeChunkHeaderBytes;
constexpr std::size_t kMosaicChunkHeaderBytes = 44;
constexpr std::size_t kMosaicChunkPayloadBytes = kMaxUdpPacketBytes - kMosaicChunkHeaderBytes;
constexpr std::size_t kMosaicEndHeaderBytes = 24;
constexpr std::uint32_t kMaxAcceptedJpegBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxPendingPatches = 64;

void appendU8(std::vector<u_char>& out, std::uint8_t v) { out.push_back(v); }
void appendU16(std::vector<u_char>& out, std::uint16_t v) {
    out.push_back(static_cast<u_char>(v & 0xffu));
    out.push_back(static_cast<u_char>((v >> 8) & 0xffu));
}
void appendU32(std::vector<u_char>& out, std::uint32_t v) {
    out.push_back(static_cast<u_char>(v & 0xffu));
    out.push_back(static_cast<u_char>((v >> 8) & 0xffu));
    out.push_back(static_cast<u_char>((v >> 16) & 0xffu));
    out.push_back(static_cast<u_char>((v >> 24) & 0xffu));
}
void appendFloat(std::vector<u_char>& out, float v) {
    std::uint32_t bits;
    static_assert(sizeof(bits) == sizeof(v), "32-bit float required");
    std::memcpy(&bits, &v, sizeof(bits));
    appendU32(out, bits);
}

bool readU8(const std::vector<u_char>& data, std::size_t& pos, std::uint8_t& v) {
    if (pos + 1 > data.size()) return false;
    v = data[pos++]; return true;
}
bool readU16(const std::vector<u_char>& data, std::size_t& pos, std::uint16_t& v) {
    if (pos + 2 > data.size()) return false;
    v = static_cast<std::uint16_t>(data[pos]) |
        (static_cast<std::uint16_t>(data[pos + 1]) << 8);
    pos += 2; return true;
}
bool readU32(const std::vector<u_char>& data, std::size_t& pos, std::uint32_t& v) {
    if (pos + 4 > data.size()) return false;
    v = static_cast<std::uint32_t>(data[pos]) |
        (static_cast<std::uint32_t>(data[pos + 1]) << 8) |
        (static_cast<std::uint32_t>(data[pos + 2]) << 16) |
        (static_cast<std::uint32_t>(data[pos + 3]) << 24);
    pos += 4; return true;
}
bool readFloat(const std::vector<u_char>& data, std::size_t& pos, float& v) {
    std::uint32_t bits;
    if (!readU32(data, pos, bits)) return false;
    std::memcpy(&v, &bits, sizeof(v));
    return true;
}

void appendCommonHeader(std::vector<u_char>& out, std::uint8_t type,
                        std::uint16_t header_bytes, std::uint32_t frame_id,
                        std::uint32_t keyframe_id, const cv::Size& original_size) {
    appendU32(out, kMagic); appendU8(out, kVersion); appendU8(out, type);
    appendU16(out, header_bytes); appendU32(out, frame_id); appendU32(out, keyframe_id);
    appendU16(out, static_cast<std::uint16_t>(original_size.width));
    appendU16(out, static_cast<std::uint16_t>(original_size.height));
}

struct CommonHeader {
    std::uint8_t type = 0;
    std::uint16_t header_bytes = 0;
    std::uint32_t frame_id = 0;
    std::uint32_t keyframe_id = 0;
    cv::Size original_size;
};

bool readCommonHeader(const std::vector<u_char>& data, std::size_t& pos, CommonHeader& h) {
    std::uint32_t magic = 0; std::uint8_t version = 0;
    std::uint16_t width = 0, height = 0;
    if (!readU32(data, pos, magic) || !readU8(data, pos, version) ||
        !readU8(data, pos, h.type) || !readU16(data, pos, h.header_bytes) ||
        !readU32(data, pos, h.frame_id) || !readU32(data, pos, h.keyframe_id) ||
        !readU16(data, pos, width) || !readU16(data, pos, height)) return false;
    if (magic != kMagic || version != kVersion || h.header_bytes < kCommonHeaderBytes) return false;
    h.original_size = cv::Size(width, height);
    return width > 0 && height > 0;
}

bool frameIdNewer(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::int32_t>(a - b) > 0;
}

cv::Mat toGray8(const cv::Mat& image) {
    cv::Mat src8;
    if (image.depth() == CV_8U) src8 = image; else image.convertTo(src8, CV_8U);
    cv::Mat gray;
    if (src8.channels() == 1) gray = src8;
    else if (src8.channels() == 3) cv::cvtColor(src8, gray, cv::COLOR_BGR2GRAY);
    else if (src8.channels() == 4) cv::cvtColor(src8, gray, cv::COLOR_BGRA2GRAY);
    return gray;
}

cv::Mat jpegInput8(const cv::Mat& image) {
    cv::Mat src8;
    if (image.depth() == CV_8U) src8 = image; else image.convertTo(src8, CV_8U);
    if (src8.channels() == 1 || src8.channels() == 3) return src8;
    cv::Mat bgr;
    if (src8.channels() == 4) cv::cvtColor(src8, bgr, cv::COLOR_BGRA2BGR);
    return bgr;
}

double brightnessGainTo128(const cv::Mat& gray) {
    const double mean_brightness = cv::mean(gray)[0];
    return mean_brightness > 1e-6 ? kTargetBrightness / mean_brightness : 1.0;
}

cv::Mat normalizeGrayForLk(const cv::Mat& gray) {
    cv::Scalar mean, stddev;
    cv::meanStdDev(gray, mean, stddev);
    cv::Mat normalized;
    if (stddev[0] <= 1e-6) {
        normalized = cv::Mat(gray.size(), CV_8U, cv::Scalar(kTargetBrightness));
        return normalized;
    }
    const double scale = kTargetLkStdDev / stddev[0];
    const double shift = kTargetBrightness - mean[0] * scale;
    gray.convertTo(normalized, CV_8U, scale, shift);
    return normalized;
}

cv::Mat normalizeColorBrightness(const cv::Mat& image, double gain) {
    const cv::Mat source = jpegInput8(image);
    if (source.empty()) return cv::Mat();
    cv::Mat normalized;
    source.convertTo(normalized, source.type(), gain);
    return normalized;
}

std::vector<cv::Point2f> selectGridFeatures(const cv::Mat& gray, EncoderTiming& timing) {
    constexpr int target_count = kFeatureGridX * kFeatureGridY * kFeaturesPerCell;
    constexpr int candidate_count = target_count * kFeatureCandidateMultiplier;

    auto stage = ProfileClock::now();
    cv::Mat detector = gray;
    int detector_scale = 1;
    while (std::max(detector.cols, detector.rows) >= kFeatureDetectorMaxSide) {
        cv::Mat smaller;
        cv::pyrDown(detector, smaller);
        detector = std::move(smaller);
        detector_scale *= 2;
    }
    timing.feature_downsample_ms += profileMs(stage);

    stage = ProfileClock::now();
    std::vector<cv::Point2f> candidates;
    candidates.reserve(candidate_count);
    const double min_distance = std::max(2.0, 7.0 / static_cast<double>(detector_scale));
    cv::goodFeaturesToTrack(detector, candidates, candidate_count,
                            0.01, min_distance, cv::noArray(), 7, false, 0.04);
    timing.feature_gftt_ms += profileMs(stage);

    stage = ProfileClock::now();
    std::array<unsigned char, kFeatureGridX * kFeatureGridY> cell_counts{};
    std::vector<cv::Point2f> points;
    points.reserve(target_count);

    for (cv::Point2f p : candidates) {
        p.x = std::clamp(p.x * detector_scale, 0.0f, static_cast<float>(gray.cols - 1));
        p.y = std::clamp(p.y * detector_scale, 0.0f, static_cast<float>(gray.rows - 1));

        const int gx = std::clamp(
            static_cast<int>(p.x * kFeatureGridX / std::max(1, gray.cols)),
            0, kFeatureGridX - 1);
        const int gy = std::clamp(
            static_cast<int>(p.y * kFeatureGridY / std::max(1, gray.rows)),
            0, kFeatureGridY - 1);
        const int cell = gy * kFeatureGridX + gx;
        if (cell_counts[cell] >= kFeaturesPerCell) continue;

        points.push_back(p);
        ++cell_counts[cell];
        if (static_cast<int>(points.size()) == target_count) break;
    }
    timing.feature_bucket_ms += profileMs(stage);
    return points;
}

double huberWeight(double error_pixels, double delta_pixels) {
    if (error_pixels <= delta_pixels || error_pixels <= 1e-12) return 1.0;
    return delta_pixels / error_pixels;
}

bool solveWeightedAffineNormalized(const std::vector<cv::Point2f>& p0,
                                   const std::vector<cv::Point2f>& p1,
                                   const std::vector<double>& weights,
                                   double cx, double cy, double scale,
                                   std::array<double, 6>& a) {
    if (p0.size() != p1.size() || p0.size() < 3 || weights.size() != p0.size()) return false;

    cv::Mat ata = cv::Mat::zeros(3, 3, CV_64F);
    cv::Mat atbu = cv::Mat::zeros(3, 1, CV_64F);
    cv::Mat atbv = cv::Mat::zeros(3, 1, CV_64F);

    for (std::size_t i = 0; i < p0.size(); ++i) {
        const double w = std::max(1e-6, weights[i]);
        const double x = (p0[i].x - cx) / scale;
        const double y = (p0[i].y - cy) / scale;
        const double u = (p1[i].x - cx) / scale;
        const double v = (p1[i].y - cy) / scale;
        const double r[3] = {x, y, 1.0};

        for (int row = 0; row < 3; ++row) {
            atbu.at<double>(row, 0) += w * r[row] * u;
            atbv.at<double>(row, 0) += w * r[row] * v;
            for (int col = 0; col < 3; ++col)
                ata.at<double>(row, col) += w * r[row] * r[col];
        }
    }
    for (int k = 0; k < 3; ++k) ata.at<double>(k, k) += 1e-9;

    cv::Mat xu, xv;
    bool ok_u = cv::solve(ata, atbu, xu, cv::DECOMP_CHOLESKY);
    bool ok_v = cv::solve(ata, atbv, xv, cv::DECOMP_CHOLESKY);
    if (!ok_u) ok_u = cv::solve(ata, atbu, xu, cv::DECOMP_SVD);
    if (!ok_v) ok_v = cv::solve(ata, atbv, xv, cv::DECOMP_SVD);
    if (!ok_u || !ok_v) return false;

    a = {xu.at<double>(0,0), xu.at<double>(1,0), xu.at<double>(2,0),
         xv.at<double>(0,0), xv.at<double>(1,0), xv.at<double>(2,0)};
    for (double v : a) if (!std::isfinite(v)) return false;
    return true;
}

bool fitRobustFullAffine(const std::vector<cv::Point2f>& p0,
                         const std::vector<cv::Point2f>& p1,
                         const cv::Size& size,
                         cv::Mat& affine) {
    if (p0.size() != p1.size() || p0.size() < 3) return false;

    const double scale = std::max(1.0, static_cast<double>(std::max(size.width, size.height)));
    const double cx = 0.5 * std::max(0, size.width - 1);
    const double cy = 0.5 * std::max(0, size.height - 1);
    std::vector<double> weights(p0.size(), 1.0);
    std::array<double, 6> a{};

    for (int iter = 0; iter < kAffineIterations; ++iter) {
        if (!solveWeightedAffineNormalized(p0, p1, weights, cx, cy, scale, a)) return false;
        double max_weight_change = 0.0;
        for (std::size_t i = 0; i < p0.size(); ++i) {
            const double x = (p0[i].x - cx) / scale;
            const double y = (p0[i].y - cy) / scale;
            const double u = a[0] * x + a[1] * y + a[2];
            const double v = a[3] * x + a[4] * y + a[5];
            const double uo = (p1[i].x - cx) / scale;
            const double vo = (p1[i].y - cy) / scale;
            const double dx = (uo - u) * scale;
            const double dy = (vo - v) * scale;
            const double next_weight = huberWeight(std::sqrt(dx * dx + dy * dy), kAffineHuberPixels);
            max_weight_change = std::max(max_weight_change, std::abs(next_weight - weights[i]));
            weights[i] = next_weight;
        }
        if (max_weight_change < 1e-3) break;
    }
    if (!solveWeightedAffineNormalized(p0, p1, weights, cx, cy, scale, a)) return false;

    const double tx = cx - a[0] * cx - a[1] * cy + scale * a[2];
    const double ty = cy - a[3] * cx - a[4] * cy + scale * a[5];
    affine = (cv::Mat_<double>(2,3) << a[0], a[1], tx, a[3], a[4], ty);
    return cv::checkRange(affine);
}

cv::Point2f applyPatchBaseTransform(const PatchData& patch, const cv::Point2f& p) {
    const float nx = patch.affine[0] * p.x + patch.affine[1] * p.y + patch.affine[2];
    const float ny = patch.affine[3] * p.x + patch.affine[4] * p.y + patch.affine[5];
    if (!patch.homography) return cv::Point2f(nx, ny);
    const float d = patch.perspective[0] * p.x + patch.perspective[1] * p.y + 1.0f;
    if (std::abs(d) < 1e-6f) return cv::Point2f(nx, ny);
    return cv::Point2f(nx / d, ny / d);
}

double homographyError(const std::array<double, 8>& h,
                       const std::vector<cv::Point2f>& p0,
                       const std::vector<cv::Point2f>& p1,
                       double cx, double cy, double scale) {
    double cost = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < p0.size(); ++i) {
        const double x = (p0[i].x - cx) / scale;
        const double y = (p0[i].y - cy) / scale;
        const double uo = (p1[i].x - cx) / scale;
        const double vo = (p1[i].y - cy) / scale;
        const double d = h[6] * x + h[7] * y + 1.0;
        if (std::abs(d) < 0.25) return std::numeric_limits<double>::infinity();
        const double u = (h[0] * x + h[1] * y + h[2]) / d;
        const double v = (h[3] * x + h[4] * y + h[5]) / d;
        const double dx = (uo - u) * scale;
        const double dy = (vo - v) * scale;
        const double e = std::sqrt(dx * dx + dy * dy);
        cost += e <= kHomographyHuberPixels
            ? 0.5 * e * e
            : kHomographyHuberPixels * (e - 0.5 * kHomographyHuberPixels);
        ++count;
    }
    return count ? cost / static_cast<double>(count) : std::numeric_limits<double>::infinity();
}

void clampHomographyPerspective(std::array<double, 8>& h, const cv::Size& size,
                                double scale) {
    const double ex = 0.5 * std::max(0, size.width - 1) / scale;
    const double ey = 0.5 * std::max(0, size.height - 1) / scale;
    const double variation = std::abs(h[6]) * ex + std::abs(h[7]) * ey;
    if (variation > kHomographyMaxDenominatorVariation && variation > 0.0) {
        const double f = kHomographyMaxDenominatorVariation / variation;
        h[6] *= f;
        h[7] *= f;
    }
}

bool refineHomographyFromAffine(const std::vector<cv::Point2f>& p0,
                                const std::vector<cv::Point2f>& p1,
                                const cv::Mat& affine,
                                const cv::Size& size,
                                std::array<float, 6>& numerator,
                                std::array<float, 2>& perspective) {
    if (p0.size() != p1.size() || p0.size() < 8 || affine.empty()) return false;

    const double scale = std::max(1.0, static_cast<double>(std::max(size.width, size.height)));
    const double cx = 0.5 * std::max(0, size.width - 1);
    const double cy = 0.5 * std::max(0, size.height - 1);

    std::array<double, 8> initial{
        affine.at<double>(0,0), affine.at<double>(0,1),
        (affine.at<double>(0,0) * cx + affine.at<double>(0,1) * cy + affine.at<double>(0,2) - cx) / scale,
        affine.at<double>(1,0), affine.at<double>(1,1),
        (affine.at<double>(1,0) * cx + affine.at<double>(1,1) * cy + affine.at<double>(1,2) - cy) / scale,
        0.0, 0.0
    };
    std::array<double, 8> h = initial;
    double best_error = homographyError(h, p0, p1, cx, cy, scale);
    const double initial_error = best_error;
    if (!std::isfinite(best_error)) return false;

    const std::array<double, 8> prior_weight{0.25, 0.25, 0.10, 0.25, 0.25, 0.10, 4.0, 4.0};

    for (int iter = 0; iter < kHomographyIterations; ++iter) {
        cv::Mat ata = cv::Mat::zeros(8, 8, CV_64F);
        cv::Mat atb = cv::Mat::zeros(8, 1, CV_64F);
        int used = 0;

        for (std::size_t i = 0; i < p0.size(); ++i) {
            const double x = (p0[i].x - cx) / scale;
            const double y = (p0[i].y - cy) / scale;
            const double uo = (p1[i].x - cx) / scale;
            const double vo = (p1[i].y - cy) / scale;
            const double d = h[6] * x + h[7] * y + 1.0;
            if (std::abs(d) < 0.25) continue;
            const double inv_d = 1.0 / d;
            const double u = (h[0] * x + h[1] * y + h[2]) * inv_d;
            const double v = (h[3] * x + h[4] * y + h[5]) * inv_d;
            const double ru = uo - u;
            const double rv = vo - v;
            const double err_px = scale * std::sqrt(ru * ru + rv * rv);
            const double weight = huberWeight(err_px, kHomographyHuberPixels);

            const double ju[8] = {
                x * inv_d, y * inv_d, inv_d, 0.0, 0.0, 0.0,
                -u * x * inv_d, -u * y * inv_d
            };
            const double jv[8] = {
                0.0, 0.0, 0.0, x * inv_d, y * inv_d, inv_d,
                -v * x * inv_d, -v * y * inv_d
            };

            for (int r = 0; r < 8; ++r) {
                atb.at<double>(r, 0) += weight * (ju[r] * ru + jv[r] * rv);
                for (int c = 0; c < 8; ++c)
                    ata.at<double>(r, c) += weight * (ju[r] * ju[c] + jv[r] * jv[c]);
            }
            ++used;
        }
        if (used < 8) break;

        for (int k = 0; k < 8; ++k) {
            ata.at<double>(k, k) += prior_weight[k] + 1e-6;
            atb.at<double>(k, 0) += prior_weight[k] * (initial[k] - h[k]);
        }

        cv::Mat delta;
        if (!cv::solve(ata, atb, delta, cv::DECOMP_CHOLESKY) &&
            !cv::solve(ata, atb, delta, cv::DECOMP_SVD)) break;

        bool accepted = false;
        double accepted_step = 0.0;
        for (double step : {1.0, 0.5, 0.25, 0.125}) {
            std::array<double, 8> candidate = h;
            for (int k = 0; k < 8; ++k)
                candidate[k] += step * delta.at<double>(k, 0);
            clampHomographyPerspective(candidate, size, scale);
            const double error = homographyError(candidate, p0, p1, cx, cy, scale);
            if (std::isfinite(error) && error < best_error) {
                h = candidate;
                best_error = error;
                accepted = true;
                accepted_step = step;
                break;
            }
        }
        if (!accepted) break;

        double max_update = 0.0;
        for (int k = 0; k < 8; ++k)
            max_update = std::max(max_update, std::abs(accepted_step * delta.at<double>(k, 0)));
        if (max_update < 1e-7) break;
    }

    if (!(best_error < initial_error * 0.999)) return false;

    const cv::Matx33d normal_to_pixel(
        scale, 0.0, cx,
        0.0, scale, cy,
        0.0, 0.0, 1.0);
    const cv::Matx33d pixel_to_normal(
        1.0 / scale, 0.0, -cx / scale,
        0.0, 1.0 / scale, -cy / scale,
        0.0, 0.0, 1.0);
    const cv::Matx33d hn(
        h[0], h[1], h[2],
        h[3], h[4], h[5],
        h[6], h[7], 1.0);
    cv::Matx33d hp = normal_to_pixel * hn * pixel_to_normal;
    if (!std::isfinite(hp(2,2)) || std::abs(hp(2,2)) < 1e-9) return false;
    hp *= 1.0 / hp(2,2);

    for (double x : {0.0, static_cast<double>(std::max(0, size.width - 1))}) {
        for (double y : {0.0, static_cast<double>(std::max(0, size.height - 1))}) {
            const double d = hp(2,0) * x + hp(2,1) * y + 1.0;
            if (!std::isfinite(d) || d < 0.5) return false;
        }
    }

    numerator = {
        static_cast<float>(hp(0,0)), static_cast<float>(hp(0,1)), static_cast<float>(hp(0,2)),
        static_cast<float>(hp(1,0)), static_cast<float>(hp(1,1)), static_cast<float>(hp(1,2))
    };
    perspective = {static_cast<float>(hp(2,0)), static_cast<float>(hp(2,1))};
    return true;
}

cv::Point2f samplePatchMesh(const PatchData& patch, const cv::Point2f& p) {
    if (patch.grid_x < 2 || patch.grid_y < 2 || patch.original_size.width < 2 || patch.original_size.height < 2 ||
        patch.mesh.size() != static_cast<std::size_t>(patch.grid_x) * patch.grid_y)
        return cv::Point2f(0.0f, 0.0f);

    const float gx = std::clamp(
        p.x * static_cast<float>(patch.grid_x - 1) / static_cast<float>(patch.original_size.width - 1),
        0.0f, static_cast<float>(patch.grid_x - 1));
    const float gy = std::clamp(
        p.y * static_cast<float>(patch.grid_y - 1) / static_cast<float>(patch.original_size.height - 1),
        0.0f, static_cast<float>(patch.grid_y - 1));
    const int x0 = static_cast<int>(std::floor(gx));
    const int y0 = static_cast<int>(std::floor(gy));
    const int x1 = std::min(x0 + 1, static_cast<int>(patch.grid_x) - 1);
    const int y1 = std::min(y0 + 1, static_cast<int>(patch.grid_y) - 1);
    const float tx = gx - x0;
    const float ty = gy - y0;

    const cv::Point2f& d00 = patch.mesh[y0 * patch.grid_x + x0];
    const cv::Point2f& d10 = patch.mesh[y0 * patch.grid_x + x1];
    const cv::Point2f& d01 = patch.mesh[y1 * patch.grid_x + x0];
    const cv::Point2f& d11 = patch.mesh[y1 * patch.grid_x + x1];
    const cv::Point2f d0 = d00 * (1.0f - tx) + d10 * tx;
    const cv::Point2f d1 = d01 * (1.0f - tx) + d11 * tx;
    return d0 * (1.0f - ty) + d1 * ty;
}

cv::Point2f predictPointFromPatch(const PatchData& patch, const cv::Point2f& p) {
    const cv::Point2f q = applyPatchBaseTransform(patch, p);
    cv::Point2f predicted = q;
    for (int iter = 0; iter < 2; ++iter)
        predicted = q + samplePatchMesh(patch, predicted);
    return predicted;
}

int quantizeTo8(int value, int max_value) {
    value = std::max(8, value); value = (value / 8) * 8;
    if (max_value >= 8) value = std::min(value, (max_value / 8) * 8);
    return std::max(8, value);
}
cv::Size scaledSize8(const cv::Size& source, double scale) {
    scale = std::clamp(scale, 0.01, 1.0);
    return cv::Size(quantizeTo8(static_cast<int>(std::floor(source.width * scale)), source.width),
                    quantizeTo8(static_cast<int>(std::floor(source.height * scale)), source.height));
}

bool encodeJpegAtSize(const cv::Mat& source, const cv::Size& size,
                      std::vector<u_char>& jpeg) {
    cv::Mat small;
    if (source.size() == size) small = source;
    else cv::resize(source, small, size, 0.0, 0.0, cv::INTER_AREA);
    return cv::imencode(".jpg", small, jpeg, {cv::IMWRITE_JPEG_QUALITY, kJpegQuality});
}

bool encodeJpeg2000AtRate(const cv::Mat& source, int compression_x1000,
                          std::vector<u_char>& encoded) {
    const cv::Mat input = jpegInput8(source);
    if (input.empty()) return false;
    compression_x1000 = std::clamp(compression_x1000,
        kJpeg2000MinCompressionX1000, kJpeg2000MaxCompressionX1000);
    try {
        return cv::imencode(".jp2", input, encoded,
            {cv::IMWRITE_JPEG2000_COMPRESSION_X1000, compression_x1000});
    } catch (const cv::Exception&) {
        encoded.clear();
        return false;
    }
}

bool encodeJpegBounded(const cv::Mat& image, int requested_jpeg_bytes,
                       double& bytes_per_pixel_model, int& model_channels,
                       std::vector<u_char>& jpeg, cv::Size& encoded_size) {
    const cv::Mat source = jpegInput8(image);
    if (source.empty()) return false;

    const int channels = source.channels();
    if (model_channels != channels) {
        model_channels = channels;
        bytes_per_pixel_model = 0.0;
    }

    const double requested_bytes = static_cast<double>(std::max(1, requested_jpeg_bytes));
    const double lower_bytes = requested_bytes * (1.0 - kJpegSizeTolerance);
    const double upper_bytes = requested_bytes * (1.0 + kJpegSizeTolerance);
    const double target_bytes = std::max(1.0, requested_bytes * kJpegTargetFill);

    const double initial_bpp = channels == 1 ?
        kJpegInitialGrayBytesPerPixel : kJpegInitialColorBytesPerPixel;
    const double predicted_bpp = std::max(kJpegMinBytesPerPixel,
        bytes_per_pixel_model > 0.0 ? bytes_per_pixel_model : initial_bpp);
    const double target_pixels = target_bytes / predicted_bpp;
    const double initial_scale = std::sqrt(
        target_pixels / std::max(1.0, static_cast<double>(source.total())));

    cv::Size current_size = scaledSize8(source.size(), initial_scale);
    std::vector<u_char> current;
    bool first_pass_within_tolerance = false;
    bool final_within_tolerance = false;

    for (int pass = 0; pass < kJpegMaxEncodePasses; ++pass) {
        current.clear();
        if (!encodeJpegAtSize(source, current_size, current)) return false;
        if (current.empty() || current.size() > std::numeric_limits<std::uint32_t>::max()) return false;

        const double current_bytes = static_cast<double>(current.size());
        const bool within_tolerance = current_bytes >= lower_bytes && current_bytes <= upper_bytes;
        if (pass == 0) first_pass_within_tolerance = within_tolerance;
        final_within_tolerance = within_tolerance;

        if (within_tolerance || pass + 1 >= kJpegMaxEncodePasses) break;

        const double current_scale = std::min(
            static_cast<double>(current_size.width) / source.cols,
            static_cast<double>(current_size.height) / source.rows);
        const double corrected_scale = current_scale * std::sqrt(target_bytes / current_bytes);
        const cv::Size next_size = scaledSize8(source.size(), corrected_scale);
        if (next_size == current_size) break;
        current_size = next_size;
    }

    jpeg = std::move(current);
    encoded_size = current_size;

    if (first_pass_within_tolerance || final_within_tolerance) {
        const double encoded_pixels = std::max(1.0,
            static_cast<double>(encoded_size.width) * encoded_size.height);
        const double observed_bpp = static_cast<double>(jpeg.size()) / encoded_pixels;
        if (bytes_per_pixel_model <= 0.0) {
            bytes_per_pixel_model = observed_bpp;
        } else {
            bytes_per_pixel_model =
                (1.0 - kJpegModelAlpha) * bytes_per_pixel_model +
                kJpegModelAlpha * observed_bpp;
        }
    }
    return true;
}

bool encodeJpeg2000Bounded(const cv::Mat& image, int requested_bytes,
                           double& compression_rate_state, int& model_channels,
                           std::vector<u_char>& encoded, cv::Size& encoded_size,
                           int& compression_x1000) {
    const cv::Mat source = jpegInput8(image);
    if (source.empty()) return false;

    const int channels = source.channels();
    if (model_channels != channels) {
        model_channels = channels;
        compression_rate_state = 0.0;
    }

    encoded_size = source.size();
    const double requested = static_cast<double>(std::max(1, requested_bytes));
    const double target_bytes = std::max(1.0, requested * kJpegTargetFill);
    const double raw_bytes = std::max(1.0,
        static_cast<double>(source.total()) * source.elemSize());

    const int current_rate = compression_rate_state > 0.0
        ? std::clamp(static_cast<int>(std::lround(compression_rate_state)),
                     kJpeg2000MinCompressionX1000, kJpeg2000MaxCompressionX1000)
        : std::clamp(static_cast<int>(std::lround(1000.0 * target_bytes / raw_bytes)),
                     kJpeg2000MinCompressionX1000, kJpeg2000MaxCompressionX1000);

    if (!encodeJpeg2000AtRate(source, current_rate, encoded)) return false;
    if (encoded.empty() || encoded.size() > std::numeric_limits<std::uint32_t>::max()) return false;

    const double current_bytes = static_cast<double>(encoded.size());
    const double correction = std::clamp(target_bytes / current_bytes, 0.5, 2.0);
    const int next_rate = std::clamp(
        static_cast<int>(std::lround(current_rate * correction)),
        kJpeg2000MinCompressionX1000, kJpeg2000MaxCompressionX1000);

    compression_rate_state = static_cast<double>(next_rate);
    compression_x1000 = current_rate;
    return true;
}

bool encodeKeyframeAtSettings(KeyframeCodec codec, const cv::Mat& source,
                              const cv::Size& size, int codec_parameter,
                              std::vector<u_char>& encoded) {
    if (codec == KeyframeCodec::Jpeg)
        return encodeJpegAtSize(source, size, encoded);

    cv::Mat small;
    if (source.size() == size) small = source;
    else cv::resize(source, small, size, 0.0, 0.0, cv::INTER_AREA);
    return encodeJpeg2000AtRate(small, codec_parameter, encoded);
}

bool encodeKeyframeBounded(KeyframeCodec codec, const cv::Mat& image,
                           int requested_bytes, double& bytes_per_pixel_model,
                           int& model_channels, std::vector<u_char>& encoded,
                           cv::Size& encoded_size, int& codec_parameter) {
    if (codec == KeyframeCodec::Jpeg) {
        codec_parameter = kJpegQuality;
        return encodeJpegBounded(image, requested_bytes,
            bytes_per_pixel_model, model_channels, encoded, encoded_size);
    }
    return encodeJpeg2000Bounded(image, requested_bytes,
        bytes_per_pixel_model, model_channels, encoded, encoded_size, codec_parameter);
}

bool mosaicEligible(const cv::Size& size) {
    return size.width >= 16 && size.height >= 16 &&
           (size.width & 1) == 0 && (size.height & 1) == 0;
}

bool stripsEligible(const cv::Size& size) {
    return size.width >= 16 && size.height >= 8 && (size.width & 1) == 0;
}

bool layeredEligible(std::uint8_t layer_count, const cv::Size& size) {
    if (layer_count == kStripsLayerCount) return stripsEligible(size);
    if (layer_count == kMosaicLayerCount) return mosaicEligible(size);
    return false;
}

std::vector<cv::Point> mosaicDPositions(const cv::Size& size) {
    std::vector<cv::Point> positions;
    if (!mosaicEligible(size)) return positions;
    const int mw = size.width / 2;
    const int mh = size.height / 2;
    const std::size_t wanted = static_cast<std::size_t>(mw) * mh;
    positions.reserve(wanted);

    if (size.width == size.height) {
        for (int py = 0; py < mh; ++py) {
            for (int px = 0; px < mw; ++px) {
                const int x = px + py + 1;
                const int y = px - py + size.height / 2;
                positions.emplace_back(x, y);
            }
        }
        return positions;
    }

    struct Candidate { long long score; cv::Point p; };
    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<std::size_t>(size.width) * size.height / 2);
    const int cx = size.width / 2;
    const int cy = size.height / 2;
    for (int y = 0; y < size.height; ++y) {
        for (int x = 0; x < size.width; ++x) {
            if (((x + y) & 1) == 0) continue;
            const long long score =
                static_cast<long long>(std::abs(x - cx)) * size.height +
                static_cast<long long>(std::abs(y - cy)) * size.width;
            candidates.push_back({score, cv::Point(x, y)});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score < b.score;
        if (a.p.y != b.p.y) return a.p.y < b.p.y;
        return a.p.x < b.p.x;
    });
    if (candidates.size() < wanted) return {};
    candidates.resize(wanted);

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        const int au = a.p.x + a.p.y, bu = b.p.x + b.p.y;
        if (au != bu) return au < bu;
        const int av = a.p.x - a.p.y, bv = b.p.x - b.p.y;
        return av < bv;
    });
    for (const Candidate& c : candidates) positions.push_back(c.p);
    return positions;
}

bool buildMosaicLayers(const cv::Mat& image, std::array<cv::Mat, 3>& layers) {
    const cv::Mat source = jpegInput8(image);
    if (source.empty() || !mosaicEligible(source.size())) return false;
    const int mw = source.cols / 2;
    const int mh = source.rows / 2;
    const cv::Size mosaic_size(mw, mh);
    for (cv::Mat& layer : layers) layer.create(mosaic_size, source.type());

    const std::size_t elem = source.elemSize();
    for (int y = 0; y < mh; ++y) {
        for (int x = 0; x < mw; ++x) {
            std::memcpy(layers[0].ptr(y) + static_cast<std::size_t>(x) * elem,
                        source.ptr(2 * y) + static_cast<std::size_t>(2 * x) * elem, elem);
            std::memcpy(layers[1].ptr(y) + static_cast<std::size_t>(x) * elem,
                        source.ptr(2 * y + 1) + static_cast<std::size_t>(2 * x + 1) * elem, elem);
        }
    }

    const std::vector<cv::Point> d_positions = mosaicDPositions(source.size());
    if (d_positions.size() != static_cast<std::size_t>(mw) * mh) return false;
    for (std::size_t i = 0; i < d_positions.size(); ++i) {
        const int py = static_cast<int>(i / mw);
        const int px = static_cast<int>(i % mw);
        const cv::Point p = d_positions[i];
        std::memcpy(layers[2].ptr(py) + static_cast<std::size_t>(px) * elem,
                    source.ptr(p.y) + static_cast<std::size_t>(p.x) * elem, elem);
    }
    return true;
}

bool buildStripsLayers(const cv::Mat& image, std::array<cv::Mat, 2>& layers) {
    const cv::Mat source = jpegInput8(image);
    if (source.empty() || !stripsEligible(source.size())) return false;

    const int sw = source.cols / 2;
    const int sh = source.rows;
    for (cv::Mat& layer : layers) layer.create(cv::Size(sw, sh), source.type());

    const std::size_t elem = source.elemSize();
    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            std::memcpy(layers[0].ptr(y) + static_cast<std::size_t>(x) * elem,
                        source.ptr(y) + static_cast<std::size_t>(2 * x) * elem, elem);
            std::memcpy(layers[1].ptr(y) + static_cast<std::size_t>(x) * elem,
                        source.ptr(y) + static_cast<std::size_t>(2 * x + 1) * elem, elem);
        }
    }
    return true;
}

std::vector<u_char> serializeKeyframeChunk(std::uint32_t frame_id, const cv::Size& original_size,
                                           const cv::Size& jpeg_size, std::uint32_t jpeg_bytes,
                                           std::uint16_t chunk_index, std::uint16_t chunk_count,
                                           std::uint32_t chunk_offset, const u_char* chunk_data,
                                           std::uint16_t chunk_bytes) {
    std::vector<u_char> packet; packet.reserve(kKeyframeChunkHeaderBytes + chunk_bytes);
    appendCommonHeader(packet, kPacketKeyframeChunk, static_cast<std::uint16_t>(kKeyframeChunkHeaderBytes),
                       frame_id, frame_id, original_size);
    appendU16(packet, static_cast<std::uint16_t>(jpeg_size.width));
    appendU16(packet, static_cast<std::uint16_t>(jpeg_size.height));
    appendU16(packet, chunk_index); appendU16(packet, chunk_count);
    appendU32(packet, jpeg_bytes); appendU32(packet, chunk_offset);
    appendU16(packet, chunk_bytes); appendU16(packet, 0);
    packet.insert(packet.end(), chunk_data, chunk_data + chunk_bytes);
    return packet;
}

std::vector<u_char> serializeMosaicKeyframeChunk(std::uint32_t frame_id, const cv::Size& original_size,
                                                 std::uint8_t layer_index, std::uint8_t layer_count,
                                                 const cv::Size& jpeg_size,
                                                 std::uint32_t jpeg_bytes, std::uint16_t chunk_index,
                                                 std::uint16_t chunk_count, std::uint32_t chunk_offset,
                                                 const u_char* chunk_data, std::uint16_t chunk_bytes) {
    std::vector<u_char> packet; packet.reserve(kMosaicChunkHeaderBytes + chunk_bytes);
    appendCommonHeader(packet, kPacketMosaicKeyframeChunk, static_cast<std::uint16_t>(kMosaicChunkHeaderBytes),
                       frame_id, frame_id, original_size);
    appendU8(packet, layer_index); appendU8(packet, layer_count); appendU16(packet, 0);
    appendU16(packet, static_cast<std::uint16_t>(jpeg_size.width));
    appendU16(packet, static_cast<std::uint16_t>(jpeg_size.height));
    appendU16(packet, chunk_index); appendU16(packet, chunk_count);
    appendU32(packet, jpeg_bytes); appendU32(packet, chunk_offset);
    appendU16(packet, chunk_bytes); appendU16(packet, 0);
    packet.insert(packet.end(), chunk_data, chunk_data + chunk_bytes);
    return packet;
}

std::vector<u_char> serializeMosaicKeyframeEnd(std::uint32_t frame_id, const cv::Size& original_size,
                                               std::uint8_t layer_count) {
    std::vector<u_char> packet; packet.reserve(kMosaicEndHeaderBytes);
    appendCommonHeader(packet, kPacketMosaicKeyframeEnd, static_cast<std::uint16_t>(kMosaicEndHeaderBytes),
                       frame_id, frame_id, original_size);
    appendU8(packet, layer_count); appendU8(packet, 0); appendU16(packet, 0);
    return packet;
}

std::vector<u_char> serializePatch(const PatchData& patch) {
    const std::size_t mesh_bytes = patch.mesh.size() * 2 * sizeof(float);
    const std::size_t perspective_bytes = patch.homography ? 2 * sizeof(float) : 0;
    const std::size_t header_bytes = kCommonHeaderBytes + 4 + 6 * sizeof(float) + perspective_bytes + mesh_bytes;
    std::vector<u_char> packet; packet.reserve(header_bytes);
    appendCommonHeader(packet, kPacketPatch, static_cast<std::uint16_t>(header_bytes),
                       patch.frame_id, patch.keyframe_id, patch.original_size);
    const std::uint16_t flags = patch.homography ? kPatchFlagHomography : 0;
    appendU8(packet, patch.grid_x); appendU8(packet, patch.grid_y); appendU16(packet, flags);
    for (float v : patch.affine) appendFloat(packet, v);
    if (patch.homography)
        for (float v : patch.perspective) appendFloat(packet, v);
    for (const cv::Point2f& p : patch.mesh) { appendFloat(packet, p.x); appendFloat(packet, p.y); }
    return packet;
}

bool parsePatch(const std::vector<u_char>& data, const CommonHeader& h, std::size_t pos, PatchData& p) {
    std::uint8_t gx = 0, gy = 0; std::uint16_t flags = 0;
    if (!readU8(data, pos, gx) || !readU8(data, pos, gy) || !readU16(data, pos, flags) || gx == 0 || gy == 0) return false;
    if ((flags & ~kPatchKnownFlags) != 0) return false;
    const bool homography = (flags & kPatchFlagHomography) != 0;
    const std::size_t mesh_count = static_cast<std::size_t>(gx) * gy;
    const std::size_t perspective_bytes = homography ? 2 * sizeof(float) : 0;
    const std::size_t expected = kCommonHeaderBytes + 4 + 6 * sizeof(float) + perspective_bytes + mesh_count * 2 * sizeof(float);
    if (h.header_bytes != expected || expected != data.size()) return false;
    p.frame_id = h.frame_id; p.keyframe_id = h.keyframe_id; p.original_size = h.original_size; p.grid_x = gx; p.grid_y = gy;
    p.homography = homography;
    for (float& v : p.affine) if (!readFloat(data, pos, v)) return false;
    p.perspective = {0.0f, 0.0f};
    if (homography)
        for (float& v : p.perspective) if (!readFloat(data, pos, v)) return false;
    p.mesh.resize(mesh_count);
    for (cv::Point2f& v : p.mesh) if (!readFloat(data, pos, v.x) || !readFloat(data, pos, v.y)) return false;
    return true;
}

template<int Channels>
cv::Mat fillOutsideImpl(const cv::Mat& image, const cv::Mat& valid) {
    cv::Mat out = image.clone();
    if (cv::countNonZero(valid) == 0) return out;
    cv::Mat state(image.size(), CV_8U, cv::Scalar(0));
    for (int y = 0; y < image.rows; ++y) {
        const unsigned char* vm = valid.ptr<unsigned char>(y);
        unsigned char* sm = state.ptr<unsigned char>(y);
        for (int x = 0; x < image.cols; ++x) if (vm[x]) sm[x] = 2;
    }
    static const int dx[8] = {-1,0,1,-1,1,-1,0,1};
    static const int dy[8] = {-1,-1,-1,0,0,1,1,1};
    std::vector<cv::Point> queue; queue.reserve(image.total());
    for (int y = 0; y < image.rows; ++y) for (int x = 0; x < image.cols; ++x) {
        if (state.at<unsigned char>(y,x) != 0) continue;
        for (int k = 0; k < 8; ++k) {
            const int nx=x+dx[k], ny=y+dy[k];
            if (nx<0 || ny<0 || nx>=image.cols || ny>=image.rows) continue;
            if (state.at<unsigned char>(ny,nx) == 2) { state.at<unsigned char>(y,x)=1; queue.emplace_back(x,y); break; }
        }
    }
    std::size_t head=0;
    while (head < queue.size()) {
        const cv::Point p=queue[head++];
        if constexpr (Channels == 1) {
            int sum=0,count=0;
            for (int k=0;k<8;++k) { const int nx=p.x+dx[k],ny=p.y+dy[k];
                if(nx<0||ny<0||nx>=image.cols||ny>=image.rows) continue;
                if(state.at<unsigned char>(ny,nx)==2){sum+=out.at<unsigned char>(ny,nx);++count;}}
            if(count) out.at<unsigned char>(p.y,p.x)=static_cast<unsigned char>((sum+count/2)/count);
        } else {
            cv::Vec3i sum(0,0,0); int count=0;
            for (int k=0;k<8;++k) { const int nx=p.x+dx[k],ny=p.y+dy[k];
                if(nx<0||ny<0||nx>=image.cols||ny>=image.rows) continue;
                if(state.at<unsigned char>(ny,nx)==2){const cv::Vec3b v=out.at<cv::Vec3b>(ny,nx);sum+=cv::Vec3i(v[0],v[1],v[2]);++count;}}
            if(count) out.at<cv::Vec3b>(p.y,p.x)=cv::Vec3b((sum[0]+count/2)/count,(sum[1]+count/2)/count,(sum[2]+count/2)/count);
        }
        state.at<unsigned char>(p.y,p.x)=2;
        for(int k=0;k<8;++k){const int nx=p.x+dx[k],ny=p.y+dy[k];
            if(nx<0||ny<0||nx>=image.cols||ny>=image.rows) continue;
            unsigned char& st=state.at<unsigned char>(ny,nx); if(st==0){st=1;queue.emplace_back(nx,ny);}}
    }
    cv::Mat previous=out.clone();
    for(int pass=0;pass<2;++pass){out.copyTo(previous);
        for(int y=0;y<image.rows;++y) for(int x=0;x<image.cols;++x){
            if(valid.at<unsigned char>(y,x)) continue;
            if constexpr(Channels==1){int sum=0,count=0;for(int k=0;k<8;++k){const int nx=x+dx[k],ny=y+dy[k];if(nx<0||ny<0||nx>=image.cols||ny>=image.rows)continue;sum+=previous.at<unsigned char>(ny,nx);++count;}if(count)out.at<unsigned char>(y,x)=static_cast<unsigned char>((sum+count/2)/count);}
            else{cv::Vec3i sum(0,0,0);int count=0;for(int k=0;k<8;++k){const int nx=x+dx[k],ny=y+dy[k];if(nx<0||ny<0||nx>=image.cols||ny>=image.rows)continue;const cv::Vec3b v=previous.at<cv::Vec3b>(ny,nx);sum+=cv::Vec3i(v[0],v[1],v[2]);++count;}if(count)out.at<cv::Vec3b>(y,x)=cv::Vec3b((sum[0]+count/2)/count,(sum[1]+count/2)/count,(sum[2]+count/2)/count);}
        }}
    return out;
}
cv::Mat fillOutside(const cv::Mat& image, const cv::Mat& valid) {
    if (cv::countNonZero(valid) == static_cast<int>(valid.total())) return image;
    if (image.type() == CV_8UC1) return fillOutsideImpl<1>(image, valid);
    if (image.type() == CV_8UC3) return fillOutsideImpl<3>(image, valid);
    return image;
}

} // namespace

void Encoder::setReference(const cv::Mat& gray, std::uint32_t frame_id) {
    const auto features_start = ProfileClock::now();
    auto stage = ProfileClock::now();
    reference_gray_ = gray.clone();
    last_timing_.feature_copy_ms += profileMs(stage);
    reference_features_ = selectGridFeatures(reference_gray_, last_timing_);
    last_timing_.features_ms += profileMs(features_start);

    stage = ProfileClock::now();
    reference_pyramid_.clear();
    reference_max_level_ = cv::buildOpticalFlowPyramid(reference_gray_, reference_pyramid_,
        kLkWindow, kLkMaxLevel, true, cv::BORDER_REFLECT_101, cv::BORDER_CONSTANT, false);
    last_timing_.ref_pyramid_ms += profileMs(stage);

    current_pyramid_.clear();
    lk_forward_.clear(); lk_back_.clear();
    lk_status_forward_.clear(); lk_status_back_.clear();
    lk_error_forward_.clear(); lk_error_back_.clear();
    previous_patch_ = PatchData{}; have_previous_patch_ = false;
    keyframe_id_ = frame_id; frames_since_keyframe_ = 0; have_reference_ = true;
}

bool Encoder::emitMosaicKeyframe(const cv::Mat& image, const cv::Mat& gray,
                                 int desired_jpeg_size, std::uint32_t frame_id) {
    std::array<cv::Mat, 3> layers;
    if (!buildMosaicLayers(image, layers)) return false;

    last_timing_.keyframe = true;
    last_timing_.keyframe_codec = keyframe_codec_;
    std::array<std::vector<u_char>, 3> jpegs;
    cv::Size jpeg_size;
    int codec_parameter = 0;
    const int layer_target = std::max(1, desired_jpeg_size / static_cast<int>(kMosaicLayerCount));

    auto stage = ProfileClock::now();
    if (!encodeKeyframeBounded(keyframe_codec_, layers[0], layer_target,
                               mosaic_jpeg_bytes_per_pixel_, mosaic_jpeg_model_channels_,
                               jpegs[0], jpeg_size, codec_parameter)) {
        last_timing_.jpeg_ms += profileMs(stage);
        return false;
    }
    if (!encodeKeyframeAtSettings(keyframe_codec_, layers[1], jpeg_size, codec_parameter, jpegs[1]) ||
        !encodeKeyframeAtSettings(keyframe_codec_, layers[2], jpeg_size, codec_parameter, jpegs[2])) {
        last_timing_.jpeg_ms += profileMs(stage);
        return false;
    }
    last_timing_.jpeg_ms += profileMs(stage);

    for (const auto& jpeg : jpegs)
        if (jpeg.empty() || jpeg.size() > std::numeric_limits<std::uint32_t>::max()) return false;

    last_timing_.mosaic_keyframe = true;
    last_timing_.jpeg_size = jpeg_size;
    last_timing_.jpeg_quality = keyframe_codec_ == KeyframeCodec::Jpeg ? kJpegQuality : 0;
    last_timing_.jpeg2000_compression_x1000 =
        keyframe_codec_ == KeyframeCodec::Jpeg2000 ? codec_parameter : 0;
    for (std::size_t i = 0; i < jpegs.size(); ++i)
        last_timing_.jpeg_layer_bytes[i] = jpegs[i].size();

    std::vector<std::vector<u_char>> packets;
    stage = ProfileClock::now();
    for (std::uint8_t layer_index = 0; layer_index < kMosaicLayerCount; ++layer_index) {
        const std::vector<u_char>& jpeg = jpegs[layer_index];
        const std::size_t count_size = (jpeg.size() + kMosaicChunkPayloadBytes - 1) / kMosaicChunkPayloadBytes;
        if (count_size == 0 || count_size > std::numeric_limits<std::uint16_t>::max()) {
            last_timing_.chunk_ms += profileMs(stage);
            return false;
        }
        const std::uint16_t chunk_count = static_cast<std::uint16_t>(count_size);
        const std::uint32_t jpeg_bytes = static_cast<std::uint32_t>(jpeg.size());
        for (std::uint16_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
            const std::size_t offset = static_cast<std::size_t>(chunk_index) * kMosaicChunkPayloadBytes;
            const std::size_t bytes = std::min(kMosaicChunkPayloadBytes, jpeg.size() - offset);
            std::vector<u_char> packet = serializeMosaicKeyframeChunk(
                frame_id, image.size(), layer_index, kMosaicLayerCount, jpeg_size, jpeg_bytes,
                chunk_index, chunk_count, static_cast<std::uint32_t>(offset),
                jpeg.data() + offset, static_cast<std::uint16_t>(bytes));
            if (packet.size() > kMaxUdpPacketBytes) {
                last_timing_.chunk_ms += profileMs(stage);
                return false;
            }
            packets.push_back(std::move(packet));
        }
    }
    packets.push_back(serializeMosaicKeyframeEnd(frame_id, image.size(), kMosaicLayerCount));
    for (auto& packet : packets) output_queue_.push_back(std::move(packet));
    last_timing_.chunk_ms += profileMs(stage);

    input_size_ = image.size(); setReference(gray, frame_id); return true;
}

bool Encoder::emitStripsKeyframe(const cv::Mat& image, const cv::Mat& gray,
                                 int desired_jpeg_size, std::uint32_t frame_id) {
    std::array<cv::Mat, 2> layers;
    if (!buildStripsLayers(image, layers)) return false;

    last_timing_.keyframe = true;
    last_timing_.keyframe_codec = keyframe_codec_;
    std::array<std::vector<u_char>, 2> jpegs;
    cv::Size jpeg_size;
    int codec_parameter = 0;
    const int layer_target = std::max(1, desired_jpeg_size / static_cast<int>(kStripsLayerCount));

    auto stage = ProfileClock::now();
    if (!encodeKeyframeBounded(keyframe_codec_, layers[0], layer_target,
                               strips_jpeg_bytes_per_pixel_, strips_jpeg_model_channels_,
                               jpegs[0], jpeg_size, codec_parameter)) {
        last_timing_.jpeg_ms += profileMs(stage);
        return false;
    }
    if (!encodeKeyframeAtSettings(keyframe_codec_, layers[1], jpeg_size, codec_parameter, jpegs[1])) {
        last_timing_.jpeg_ms += profileMs(stage);
        return false;
    }
    last_timing_.jpeg_ms += profileMs(stage);

    for (const auto& jpeg : jpegs)
        if (jpeg.empty() || jpeg.size() > std::numeric_limits<std::uint32_t>::max()) return false;

    last_timing_.strips_keyframe = true;
    last_timing_.jpeg_size = jpeg_size;
    last_timing_.jpeg_quality = keyframe_codec_ == KeyframeCodec::Jpeg ? kJpegQuality : 0;
    last_timing_.jpeg2000_compression_x1000 =
        keyframe_codec_ == KeyframeCodec::Jpeg2000 ? codec_parameter : 0;
    for (std::size_t i = 0; i < jpegs.size(); ++i)
        last_timing_.jpeg_layer_bytes[i] = jpegs[i].size();

    std::vector<std::vector<u_char>> packets;
    stage = ProfileClock::now();
    for (std::uint8_t layer_index = 0; layer_index < kStripsLayerCount; ++layer_index) {
        const std::vector<u_char>& jpeg = jpegs[layer_index];
        const std::size_t count_size = (jpeg.size() + kMosaicChunkPayloadBytes - 1) / kMosaicChunkPayloadBytes;
        if (count_size == 0 || count_size > std::numeric_limits<std::uint16_t>::max()) {
            last_timing_.chunk_ms += profileMs(stage);
            return false;
        }
        const std::uint16_t chunk_count = static_cast<std::uint16_t>(count_size);
        const std::uint32_t jpeg_bytes = static_cast<std::uint32_t>(jpeg.size());
        for (std::uint16_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
            const std::size_t offset = static_cast<std::size_t>(chunk_index) * kMosaicChunkPayloadBytes;
            const std::size_t bytes = std::min(kMosaicChunkPayloadBytes, jpeg.size() - offset);
            std::vector<u_char> packet = serializeMosaicKeyframeChunk(
                frame_id, image.size(), layer_index, kStripsLayerCount, jpeg_size, jpeg_bytes,
                chunk_index, chunk_count, static_cast<std::uint32_t>(offset),
                jpeg.data() + offset, static_cast<std::uint16_t>(bytes));
            if (packet.size() > kMaxUdpPacketBytes) {
                last_timing_.chunk_ms += profileMs(stage);
                return false;
            }
            packets.push_back(std::move(packet));
        }
    }
    packets.push_back(serializeMosaicKeyframeEnd(frame_id, image.size(), kStripsLayerCount));
    for (auto& packet : packets) output_queue_.push_back(std::move(packet));
    last_timing_.chunk_ms += profileMs(stage);

    input_size_ = image.size(); setReference(gray, frame_id); return true;
}

bool Encoder::emitKeyframe(const cv::Mat& image, const cv::Mat& gray,
                           int desired_jpeg_size, std::uint32_t frame_id) {
    if (strips_keyframes_ && stripsEligible(image.size())) {
        if (emitStripsKeyframe(image, gray, desired_jpeg_size, frame_id)) return true;
    }
    if (mosaic_keyframes_ && mosaicEligible(image.size())) {
        if (emitMosaicKeyframe(image, gray, desired_jpeg_size, frame_id)) return true;
    }

    last_timing_.keyframe = true;
    last_timing_.keyframe_codec = keyframe_codec_;
    std::vector<u_char> jpeg; cv::Size jpeg_size;
    int codec_parameter = 0;

    auto stage = ProfileClock::now();
    const bool jpeg_ok = encodeKeyframeBounded(
        keyframe_codec_, image, desired_jpeg_size,
        jpeg_bytes_per_pixel_, jpeg_model_channels_, jpeg, jpeg_size, codec_parameter);
    last_timing_.jpeg_ms += profileMs(stage);
    if (!jpeg_ok) return false;
    last_timing_.jpeg_layer_bytes[0] = jpeg.size();
    last_timing_.jpeg_size = jpeg_size;
    last_timing_.jpeg_quality = keyframe_codec_ == KeyframeCodec::Jpeg ? kJpegQuality : 0;
    last_timing_.jpeg2000_compression_x1000 =
        keyframe_codec_ == KeyframeCodec::Jpeg2000 ? codec_parameter : 0;

    const std::size_t count_size = (jpeg.size() + kKeyframeChunkPayloadBytes - 1) / kKeyframeChunkPayloadBytes;
    if (count_size == 0 || count_size > std::numeric_limits<std::uint16_t>::max()) return false;
    const std::uint16_t chunk_count = static_cast<std::uint16_t>(count_size);
    const std::uint32_t jpeg_bytes = static_cast<std::uint32_t>(jpeg.size());

    stage = ProfileClock::now();
    for (std::uint16_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const std::size_t offset = static_cast<std::size_t>(chunk_index) * kKeyframeChunkPayloadBytes;
        const std::size_t bytes = std::min(kKeyframeChunkPayloadBytes, jpeg.size() - offset);
        std::vector<u_char> packet = serializeKeyframeChunk(frame_id, image.size(), jpeg_size, jpeg_bytes,
            chunk_index, chunk_count, static_cast<std::uint32_t>(offset), jpeg.data() + offset,
            static_cast<std::uint16_t>(bytes));
        if (packet.size() > kMaxUdpPacketBytes) {
            last_timing_.chunk_ms += profileMs(stage);
            return false;
        }
        output_queue_.push_back(std::move(packet));
    }
    last_timing_.chunk_ms += profileMs(stage);

    input_size_ = image.size(); setReference(gray, frame_id); return true;
}

bool Encoder::estimatePatch(const cv::Mat& current_gray, std::uint32_t frame_id, PatchData& patch) {
    if (!have_reference_ || reference_features_.size() < 4) return false;

    auto stage = ProfileClock::now();
    current_pyramid_.clear();
    const int current_max_level = cv::buildOpticalFlowPyramid(current_gray, current_pyramid_,
        kLkWindow, kLkMaxLevel, true, cv::BORDER_REFLECT_101, cv::BORDER_CONSTANT, false);
    const int max_level = std::min(reference_max_level_, current_max_level);
    last_timing_.cur_pyramid_ms += profileMs(stage);

    lk_status_forward_.clear(); lk_error_forward_.clear();
    int forward_flags = 0;
    const bool can_predict = have_previous_patch_ &&
        previous_patch_.keyframe_id == keyframe_id_ && previous_patch_.original_size == input_size_;
    last_timing_.predictor_used = can_predict;

    stage = ProfileClock::now();
    if (can_predict) {
        lk_forward_.resize(reference_features_.size());
        for (std::size_t i = 0; i < reference_features_.size(); ++i)
            lk_forward_[i] = predictPointFromPatch(previous_patch_, reference_features_[i]);
        forward_flags = cv::OPTFLOW_USE_INITIAL_FLOW;
    } else {
        lk_forward_.clear();
    }
    last_timing_.predictor_ms += profileMs(stage);

    stage = ProfileClock::now();
    cv::calcOpticalFlowPyrLK(reference_pyramid_, current_pyramid_, reference_features_, lk_forward_,
        lk_status_forward_, lk_error_forward_, kLkWindow, max_level,
        cv::TermCriteria(cv::TermCriteria::COUNT|cv::TermCriteria::EPS,40,0.01), forward_flags, 1e-4);
    last_timing_.lk_forward_ms += profileMs(stage);

    std::vector<cv::Point2f> g0,g1; g0.reserve(reference_features_.size()); g1.reserve(reference_features_.size());
    for(std::size_t i=0;i<reference_features_.size();++i) if(i<lk_status_forward_.size()&&lk_status_forward_[i]&&i<lk_error_forward_.size()&&lk_error_forward_[i]<kLkForwardErrorMax){g0.push_back(reference_features_[i]);g1.push_back(lk_forward_[i]);}
    if(g0.size()<4) return false;

    lk_back_ = g0; lk_status_back_.clear(); lk_error_back_.clear();
    stage = ProfileClock::now();
    cv::calcOpticalFlowPyrLK(current_pyramid_, reference_pyramid_, g1, lk_back_, lk_status_back_, lk_error_back_,
        kLkWindow,max_level,cv::TermCriteria(cv::TermCriteria::COUNT|cv::TermCriteria::EPS,40,0.01),
        cv::OPTFLOW_USE_INITIAL_FLOW,1e-4);
    last_timing_.lk_backward_ms += profileMs(stage);

    std::vector<cv::Point2f> p0,p1; p0.reserve(g0.size()); p1.reserve(g0.size());
    for(std::size_t i=0;i<g0.size();++i){if(i>=lk_status_back_.size()||!lk_status_back_[i])continue;if(cv::norm(lk_back_[i]-g0[i])>kLkBackwardErrorMax)continue;p0.push_back(g0[i]);p1.push_back(g1[i]);}
    if(p0.size()<4) return false;

    stage = ProfileClock::now();
    cv::Mat affine;
    if(!fitRobustFullAffine(p0,p1,input_size_,affine)) {
        last_timing_.affine_ms += profileMs(stage);
        return false;
    }
    last_timing_.affine_ms += profileMs(stage);

    patch.frame_id=frame_id; patch.keyframe_id=keyframe_id_; patch.original_size=input_size_; patch.grid_x=kMeshGridX; patch.grid_y=kMeshGridY;
    patch.mesh.assign(kMeshGridX*kMeshGridY,cv::Point2f(0,0));
    patch.affine={static_cast<float>(affine.at<double>(0,0)),static_cast<float>(affine.at<double>(0,1)),static_cast<float>(affine.at<double>(0,2)),static_cast<float>(affine.at<double>(1,0)),static_cast<float>(affine.at<double>(1,1)),static_cast<float>(affine.at<double>(1,2))};
    patch.homography=false;
    patch.perspective={0.0f,0.0f};

    if (homography_transform_) {
        stage = ProfileClock::now();
        std::array<float, 6> numerator;
        std::array<float, 2> perspective;
        if (refineHomographyFromAffine(p0, p1, affine, input_size_, numerator, perspective)) {
            patch.affine = numerator;
            patch.perspective = perspective;
            patch.homography = true;
        }
        last_timing_.homography_ms += profileMs(stage);
    }

    stage = ProfileClock::now();
    std::vector<cv::Point2f> q,residual; q.reserve(p0.size()); residual.reserve(p0.size());
    for(std::size_t i=0;i<p0.size();++i){const cv::Point2f qi=applyPatchBaseTransform(patch,p0[i]);const cv::Point2f ri=p1[i]-qi;q.push_back(qi);residual.push_back(ri);}
    const double sigma=std::max(16.0,0.20*std::max(input_size_.width,input_size_.height)); const double inv2s2=1.0/(2.0*sigma*sigma);
    for(int iy=0;iy<kMeshGridY;++iy){const float y=static_cast<float>(iy)*(input_size_.height-1)/(kMeshGridY-1);
        for(int ix=0;ix<kMeshGridX;++ix){const float x=static_cast<float>(ix)*(input_size_.width-1)/(kMeshGridX-1);cv::Point2d sum(0,0);double sw=0;
            for(std::size_t k=0;k<q.size();++k){const double dx=q[k].x-x,dy=q[k].y-y,w=std::exp(-(dx*dx+dy*dy)*inv2s2);sum.x+=w*residual[k].x;sum.y+=w*residual[k].y;sw+=w;}
            if(sw>1e-9)patch.mesh[iy*kMeshGridX+ix]=cv::Point2f(static_cast<float>(sum.x/sw),static_cast<float>(sum.y/sw));}}
    last_timing_.mesh_ms += profileMs(stage);
    return true;
}

void Encoder::pushImage(cv::Mat& image, int desired_jpeg_size, int keyframe_once_in_N) {
    last_timing_ = EncoderTiming{};
    last_timing_.keyframe_codec = keyframe_codec_;
    if (image.empty()) return;
    if (image.cols > std::numeric_limits<std::uint16_t>::max() ||
        image.rows > std::numeric_limits<std::uint16_t>::max()) return;

    const auto prep_start = ProfileClock::now();
    const cv::Mat raw_gray = toGray8(image);
    if (raw_gray.empty()) {
        last_timing_.prep_ms = profileMs(prep_start);
        return;
    }

    const double brightness_gain = brightnessGainTo128(raw_gray);
    const cv::Mat gray = normalizeGrayForLk(raw_gray);
    last_timing_.prep_ms = profileMs(prep_start);

    const std::uint32_t frame_id = next_frame_id_++;
    const int period = std::max(1, keyframe_once_in_N);
    const bool size_changed = have_reference_ && image.size() != input_size_;
    const bool periodic = !have_reference_ || period == 1 || frames_since_keyframe_ >= period - 1;

    if (size_changed)
        have_reference_ = false;

    auto emit_normalized_keyframe = [&]() {
        const auto stage = ProfileClock::now();
        cv::Mat normalized_image = normalizeColorBrightness(image, brightness_gain);
        last_timing_.color_norm_ms += profileMs(stage);
        if (!normalized_image.empty())
            emitKeyframe(normalized_image, gray, desired_jpeg_size, frame_id);
    };

    if (!have_reference_ || periodic) {
        emit_normalized_keyframe();
        return;
    }

    PatchData patch;
    if (!estimatePatch(gray, frame_id, patch)) {
        emit_normalized_keyframe();
        return;
    }

    const auto serialize_start = ProfileClock::now();
    std::vector<u_char> packet = serializePatch(patch);
    last_timing_.serialize_ms += profileMs(serialize_start);
    if (packet.size() <= kMaxUdpPacketBytes) {
        output_queue_.push_back(std::move(packet));
        previous_patch_ = patch;
        have_previous_patch_ = true;
        ++frames_since_keyframe_;
    } else {
        emit_normalized_keyframe();
    }
}

bool Encoder::getNextChunk(std::vector<u_char>& data){if(output_queue_.empty())return false;data=std::move(output_queue_.front());output_queue_.pop_front();return true;}

bool Encoder::getLastLKDebug(LKDebugData& debug) const {
    debug = LKDebugData{};
    if constexpr (!kShowLKImage) return false;
    if (!have_reference_) return false;

    if (frames_since_keyframe_ == 0) {
        if (reference_pyramid_.empty() || reference_pyramid_.front().empty()) return false;
        debug.image = reference_pyramid_.front();
        debug.from = reference_features_;
        debug.to = reference_features_;
        return true;
    }

    if (current_pyramid_.empty() || current_pyramid_.front().empty()) return false;
    debug.image = current_pyramid_.front();
    debug.from.reserve(reference_features_.size());
    debug.to.reserve(reference_features_.size());

    std::size_t backward_index = 0;
    for (std::size_t i = 0; i < reference_features_.size(); ++i) {
        const bool forward_valid =
            i < lk_status_forward_.size() && lk_status_forward_[i] &&
            i < lk_error_forward_.size() && lk_error_forward_[i] < kLkForwardErrorMax &&
            i < lk_forward_.size();
        if (!forward_valid) continue;

        if (backward_index < lk_status_back_.size() && lk_status_back_[backward_index] &&
            backward_index < lk_back_.size() &&
            cv::norm(lk_back_[backward_index] - reference_features_[i]) <= kLkBackwardErrorMax) {
            debug.from.push_back(reference_features_[i]);
            debug.to.push_back(lk_forward_[i]);
        }
        ++backward_index;
    }
    return true;
}

void Decoder::resetPendingKeyframe() {
    pending_keyframe_ = KeyframeAssembly{};
    pending_mosaic_ = MosaicAssembly{};
    pending_patch_queue_.clear();
}

bool Decoder::decodeMosaicLayer(std::uint8_t layer_index) {
    if (!pending_mosaic_.active || layer_index >= pending_mosaic_.layer_count ||
        layer_index >= pending_mosaic_.layers.size()) return false;
    MosaicLayerAssembly& layer = pending_mosaic_.layers[layer_index];
    if (layer.complete) return true;
    if (layer.bytes.empty() || layer.received_count != layer.chunk_count) return false;

    cv::Mat encoded(1, static_cast<int>(layer.bytes.size()), CV_8U, layer.bytes.data());
    const auto decode_start = ProfileClock::now();
    cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_UNCHANGED);
    pending_mosaic_.image_decode_ms += profileMs(decode_start);
    if (decoded.empty() || decoded.size() != layer.jpeg_size) return false;
    layer.decoded = decoded;
    layer.complete = true;
    return true;
}

bool Decoder::rebuildMosaicKeyframe() {
    if (!pending_mosaic_.active ||
        !layeredEligible(pending_mosaic_.layer_count, pending_mosaic_.original_size)) return false;

    cv::Size sample_size;
    int type = -1;
    for (std::uint8_t i = 0; i < pending_mosaic_.layer_count; ++i) {
        const MosaicLayerAssembly& layer = pending_mosaic_.layers[i];
        if (layer.complete && !layer.decoded.empty()) {
            sample_size = layer.decoded.size();
            type = layer.decoded.type();
            break;
        }
    }
    if (type < 0 || sample_size.width <= 0 || sample_size.height <= 0) return false;

    const bool strips = pending_mosaic_.layer_count == kStripsLayerCount;
    const cv::Size assembled_size(
        sample_size.width * 2,
        strips ? sample_size.height : sample_size.height * 2);
    if (strips) {
        if (!stripsEligible(assembled_size)) return false;
    } else {
        if (!mosaicEligible(assembled_size)) return false;
    }
    if (assembled_size.width > pending_mosaic_.original_size.width ||
        assembled_size.height > pending_mosaic_.original_size.height) return false;

    const int mw = sample_size.width;
    const int mh = sample_size.height;
    cv::Mat assembled(assembled_size, type, cv::Scalar::all(0));
    cv::Mat valid(assembled_size, CV_8U, cv::Scalar(0));
    const std::size_t elem = assembled.elemSize();

    auto layerUsable = [&](const MosaicLayerAssembly& layer) {
        return layer.complete && !layer.decoded.empty() &&
               layer.decoded.size() == sample_size && layer.decoded.type() == type;
    };
    auto put = [&](int x, int y, const cv::Mat& layer, int lx, int ly) {
        if (x < 0 || y < 0 || x >= assembled_size.width || y >= assembled_size.height) return;
        std::memcpy(assembled.ptr(y) + static_cast<std::size_t>(x) * elem,
                    layer.ptr(ly) + static_cast<std::size_t>(lx) * elem, elem);
        valid.at<unsigned char>(y, x) = 255;
    };

    const MosaicLayerAssembly& a = pending_mosaic_.layers[0];
    const MosaicLayerAssembly& b = pending_mosaic_.layers[1];
    if (strips) {
        if (layerUsable(a))
            for (int y = 0; y < mh; ++y) for (int x = 0; x < mw; ++x)
                put(2 * x, y, a.decoded, x, y);
        if (layerUsable(b))
            for (int y = 0; y < mh; ++y) for (int x = 0; x < mw; ++x)
                put(2 * x + 1, y, b.decoded, x, y);
    } else {
        if (layerUsable(a))
            for (int y = 0; y < mh; ++y) for (int x = 0; x < mw; ++x)
                put(2 * x, 2 * y, a.decoded, x, y);
        if (layerUsable(b))
            for (int y = 0; y < mh; ++y) for (int x = 0; x < mw; ++x)
                put(2 * x + 1, 2 * y + 1, b.decoded, x, y);

        const MosaicLayerAssembly& d = pending_mosaic_.layers[2];
        if (layerUsable(d)) {
            const std::vector<cv::Point> positions = mosaicDPositions(assembled_size);
            if (positions.size() != static_cast<std::size_t>(mw) * mh) return false;
            for (std::size_t i = 0; i < positions.size(); ++i) {
                const int ly = static_cast<int>(i / mw);
                const int lx = static_cast<int>(i % mw);
                put(positions[i].x, positions[i].y, d.decoded, lx, ly);
            }
        }
    }

    if (cv::countNonZero(valid) == 0) return false;
    cv::Mat reconstructed = kMosaicFillMissing ? fillOutside(assembled, valid) : assembled;
    if (reconstructed.empty()) return false;

    const bool new_keyframe = !have_keyframe_ || keyframe_id_ != pending_mosaic_.frame_id;
    original_size_ = pending_mosaic_.original_size;
    keyframe_id_ = pending_mosaic_.frame_id;
    have_keyframe_ = true;
    current_jpeg_.clear();
    decoded_keyframe_ = reconstructed;
    last_keyframe_image_decode_ms_ = pending_mosaic_.image_decode_ms;
    keyframe_changed_ = true;
    previous_render_.release();

    if (new_keyframe) {
        patch_queue_.clear();
        while (!pending_patch_queue_.empty()) {
            patch_queue_.push_back(std::move(pending_patch_queue_.front()));
            pending_patch_queue_.pop_front();
        }
    }
    return true;
}

void Decoder::pushData(const std::vector<u_char>& data){
    if(data.size()<kCommonHeaderBytes||data.size()>kMaxUdpPacketBytes)return;std::size_t pos=0;CommonHeader h;if(!readCommonHeader(data,pos,h)||h.header_bytes>data.size())return;

    if(h.type==kPacketKeyframeChunk){
        if(h.header_bytes!=kKeyframeChunkHeaderBytes||h.keyframe_id!=h.frame_id)return;
        std::uint16_t jw=0,jh=0,chunk_index=0,chunk_count=0,chunk_bytes=0,reserved=0;std::uint32_t jpeg_bytes=0,chunk_offset=0;
        if(!readU16(data,pos,jw)||!readU16(data,pos,jh)||!readU16(data,pos,chunk_index)||!readU16(data,pos,chunk_count)||
           !readU32(data,pos,jpeg_bytes)||!readU32(data,pos,chunk_offset)||!readU16(data,pos,chunk_bytes)||!readU16(data,pos,reserved))return;
        if(jw==0||jh==0||jpeg_bytes==0||jpeg_bytes>kMaxAcceptedJpegBytes||chunk_count==0||chunk_index>=chunk_count||
           static_cast<std::size_t>(h.header_bytes)+chunk_bytes!=data.size())return;
        const std::size_t expected_count=(static_cast<std::size_t>(jpeg_bytes)+kKeyframeChunkPayloadBytes-1)/kKeyframeChunkPayloadBytes;
        if(expected_count!=chunk_count)return;
        const std::size_t expected_offset=static_cast<std::size_t>(chunk_index)*kKeyframeChunkPayloadBytes;
        if(expected_offset>=jpeg_bytes)return;
        const std::size_t expected_bytes=std::min(kKeyframeChunkPayloadBytes,static_cast<std::size_t>(jpeg_bytes)-expected_offset);
        if(expected_offset!=chunk_offset||expected_bytes!=chunk_bytes||expected_offset+expected_bytes>jpeg_bytes)return;
        if(have_keyframe_){if(h.frame_id==keyframe_id_)return;if(!frameIdNewer(h.frame_id,keyframe_id_))return;}
        if(pending_mosaic_.active){if(!frameIdNewer(h.frame_id,pending_mosaic_.frame_id))return;pending_mosaic_=MosaicAssembly{};pending_patch_queue_.clear();}
        if(pending_keyframe_.active&&h.frame_id!=pending_keyframe_.frame_id){if(!frameIdNewer(h.frame_id,pending_keyframe_.frame_id))return;pending_keyframe_=KeyframeAssembly{};pending_patch_queue_.clear();}
        if(!pending_keyframe_.active){
            pending_keyframe_.active=true;pending_keyframe_.frame_id=h.frame_id;pending_keyframe_.original_size=h.original_size;
            pending_keyframe_.jpeg_size=cv::Size(jw,jh);pending_keyframe_.jpeg_bytes=jpeg_bytes;pending_keyframe_.chunk_count=chunk_count;
            pending_keyframe_.received_count=0;pending_keyframe_.bytes.resize(jpeg_bytes);pending_keyframe_.received.assign(chunk_count,0);
        }else if(pending_keyframe_.original_size!=h.original_size||pending_keyframe_.jpeg_size!=cv::Size(jw,jh)||
                 pending_keyframe_.jpeg_bytes!=jpeg_bytes||pending_keyframe_.chunk_count!=chunk_count)return;
        if(!pending_keyframe_.received[chunk_index]){
            std::copy(data.begin()+h.header_bytes,data.end(),pending_keyframe_.bytes.begin()+chunk_offset);
            pending_keyframe_.received[chunk_index]=1;++pending_keyframe_.received_count;
        }
        if(pending_keyframe_.received_count==pending_keyframe_.chunk_count){
            current_jpeg_=std::move(pending_keyframe_.bytes);original_size_=pending_keyframe_.original_size;keyframe_id_=pending_keyframe_.frame_id;
            have_keyframe_=true;keyframe_changed_=true;decoded_keyframe_.release();previous_render_.release();patch_queue_.clear();last_keyframe_image_decode_ms_=0.0;
            while(!pending_patch_queue_.empty()){patch_queue_.push_back(std::move(pending_patch_queue_.front()));pending_patch_queue_.pop_front();}
            pending_keyframe_=KeyframeAssembly{};pending_mosaic_=MosaicAssembly{};
        }
        return;
    }

    if(h.type==kPacketMosaicKeyframeChunk){
        if(h.header_bytes!=kMosaicChunkHeaderBytes||h.keyframe_id!=h.frame_id)return;
        std::uint8_t layer_index=0,layer_count=0;std::uint16_t reserved0=0,jw=0,jh=0,chunk_index=0,chunk_count=0,chunk_bytes=0,reserved1=0;std::uint32_t jpeg_bytes=0,chunk_offset=0;
        if(!readU8(data,pos,layer_index)||!readU8(data,pos,layer_count)||!readU16(data,pos,reserved0)||
           !readU16(data,pos,jw)||!readU16(data,pos,jh)||!readU16(data,pos,chunk_index)||!readU16(data,pos,chunk_count)||
           !readU32(data,pos,jpeg_bytes)||!readU32(data,pos,chunk_offset)||!readU16(data,pos,chunk_bytes)||!readU16(data,pos,reserved1))return;
        if(!layeredEligible(layer_count,h.original_size)||layer_index>=layer_count||layer_index>=pending_mosaic_.layers.size()||
           jw==0||jh==0||jpeg_bytes==0||jpeg_bytes>kMaxAcceptedJpegBytes||chunk_count==0||chunk_index>=chunk_count||
           static_cast<std::size_t>(h.header_bytes)+chunk_bytes!=data.size())return;
        const std::size_t expected_count=(static_cast<std::size_t>(jpeg_bytes)+kMosaicChunkPayloadBytes-1)/kMosaicChunkPayloadBytes;
        const std::size_t expected_offset=static_cast<std::size_t>(chunk_index)*kMosaicChunkPayloadBytes;
        if(expected_count!=chunk_count||expected_offset>=jpeg_bytes)return;
        const std::size_t expected_bytes=std::min(kMosaicChunkPayloadBytes,static_cast<std::size_t>(jpeg_bytes)-expected_offset);
        if(expected_offset!=chunk_offset||expected_bytes!=chunk_bytes||expected_offset+expected_bytes>jpeg_bytes)return;

        if(have_keyframe_&&h.frame_id!=keyframe_id_&&!frameIdNewer(h.frame_id,keyframe_id_))return;
        if(have_keyframe_&&h.frame_id==keyframe_id_&&(!pending_mosaic_.active||pending_mosaic_.frame_id!=h.frame_id))return;
        if(pending_keyframe_.active){if(!frameIdNewer(h.frame_id,pending_keyframe_.frame_id))return;pending_keyframe_=KeyframeAssembly{};pending_patch_queue_.clear();}
        if(pending_mosaic_.active&&h.frame_id!=pending_mosaic_.frame_id){if(!frameIdNewer(h.frame_id,pending_mosaic_.frame_id))return;pending_mosaic_=MosaicAssembly{};pending_patch_queue_.clear();}
        if(!pending_mosaic_.active){
            pending_mosaic_.active=true;pending_mosaic_.frame_id=h.frame_id;pending_mosaic_.original_size=h.original_size;pending_mosaic_.layer_count=layer_count;
        }else if(pending_mosaic_.original_size!=h.original_size||pending_mosaic_.layer_count!=layer_count)return;

        MosaicLayerAssembly& layer=pending_mosaic_.layers[layer_index];
        if(layer.jpeg_bytes==0){
            layer.jpeg_size=cv::Size(jw,jh);layer.jpeg_bytes=jpeg_bytes;layer.chunk_count=chunk_count;layer.received_count=0;
            layer.bytes.resize(jpeg_bytes);layer.received.assign(chunk_count,0);
        }else if(layer.jpeg_size!=cv::Size(jw,jh)||layer.jpeg_bytes!=jpeg_bytes||layer.chunk_count!=chunk_count)return;
        if(!layer.received[chunk_index]){
            std::copy(data.begin()+h.header_bytes,data.end(),layer.bytes.begin()+chunk_offset);
            layer.received[chunk_index]=1;++layer.received_count;
        }
        if(!layer.complete&&layer.received_count==layer.chunk_count){
            if(decodeMosaicLayer(layer_index)&&(pending_mosaic_.end_received||(have_keyframe_&&keyframe_id_==pending_mosaic_.frame_id)))
                rebuildMosaicKeyframe();
        }
        return;
    }

    if(h.type==kPacketMosaicKeyframeEnd){
        if(h.header_bytes!=kMosaicEndHeaderBytes||data.size()!=kMosaicEndHeaderBytes||h.keyframe_id!=h.frame_id)return;
        std::uint8_t layer_count=0,reserved0=0;std::uint16_t reserved1=0;
        if(!readU8(data,pos,layer_count)||!readU8(data,pos,reserved0)||!readU16(data,pos,reserved1)||
           !layeredEligible(layer_count,h.original_size))return;
        if(have_keyframe_&&h.frame_id!=keyframe_id_&&!frameIdNewer(h.frame_id,keyframe_id_))return;
        if(pending_keyframe_.active){if(!frameIdNewer(h.frame_id,pending_keyframe_.frame_id))return;pending_keyframe_=KeyframeAssembly{};pending_patch_queue_.clear();}
        if(pending_mosaic_.active&&h.frame_id!=pending_mosaic_.frame_id){if(!frameIdNewer(h.frame_id,pending_mosaic_.frame_id))return;pending_mosaic_=MosaicAssembly{};pending_patch_queue_.clear();}
        if(!pending_mosaic_.active){pending_mosaic_.active=true;pending_mosaic_.frame_id=h.frame_id;pending_mosaic_.original_size=h.original_size;pending_mosaic_.layer_count=layer_count;}
        else if(pending_mosaic_.original_size!=h.original_size||pending_mosaic_.layer_count!=layer_count)return;
        pending_mosaic_.end_received=true;
        rebuildMosaicKeyframe();
        return;
    }

    if(h.type==kPacketPatch){
        PatchData patch;if(!parsePatch(data,h,pos,patch))return;
        if(pending_mosaic_.active&&h.keyframe_id==pending_mosaic_.frame_id&&h.original_size==pending_mosaic_.original_size){
            if(!have_keyframe_||keyframe_id_!=pending_mosaic_.frame_id){
                if(!rebuildMosaicKeyframe()){
                    if(pending_patch_queue_.size()>=kMaxPendingPatches)pending_patch_queue_.pop_front();pending_patch_queue_.push_back(std::move(patch));return;
                }
            }
            if(have_keyframe_&&h.keyframe_id==keyframe_id_&&h.original_size==original_size_){patch_queue_.push_back(std::move(patch));return;}
        }
        if(have_keyframe_&&h.keyframe_id==keyframe_id_&&h.original_size==original_size_){patch_queue_.push_back(std::move(patch));return;}
        if(pending_keyframe_.active&&h.keyframe_id==pending_keyframe_.frame_id&&h.original_size==pending_keyframe_.original_size){
            if(pending_patch_queue_.size()>=kMaxPendingPatches)pending_patch_queue_.pop_front();pending_patch_queue_.push_back(std::move(patch));
        }
    }
}

bool Decoder::updateKeyframe(std::vector<u_char>& jpeg_data){if(!keyframe_changed_)return false;jpeg_data=current_jpeg_;keyframe_changed_=false;return true;}
bool Decoder::getNextPatch(std::vector<PatchData>& patch){if(patch_queue_.empty())return false;patch.clear();patch.push_back(std::move(patch_queue_.front()));patch_queue_.pop_front();return true;}

cv::Mat Decoder::getDecodedKeyframe(const std::vector<u_char>& jpeg_data){
    if(!have_keyframe_||original_size_.width<=0||original_size_.height<=0)return cv::Mat();
    if(!decoded_keyframe_.empty())return decoded_keyframe_;
    const std::vector<u_char>& bytes=jpeg_data.empty()?current_jpeg_:jpeg_data;if(bytes.empty())return cv::Mat();
    cv::Mat encoded(1,static_cast<int>(bytes.size()),CV_8U,const_cast<u_char*>(bytes.data()));
    const auto decode_start=ProfileClock::now();
    cv::Mat decoded=cv::imdecode(encoded,cv::IMREAD_UNCHANGED);
    last_keyframe_image_decode_ms_=profileMs(decode_start);
    if(decoded.empty())return cv::Mat();if(decoded.size()!=original_size_)cv::resize(decoded,decoded,original_size_,0,0,cv::INTER_LINEAR);decoded_keyframe_=decoded;return decoded_keyframe_;
}

void Decoder::render(cv::Mat& destination,const std::vector<PatchData>& patch,const std::vector<u_char>& jpeg_data){
    cv::Mat keyframe=getDecodedKeyframe(jpeg_data);
    if(keyframe.empty()){destination.release();return;}
    if(patch.empty()){
        if(keyframe.size()==original_size_)keyframe.copyTo(destination);
        else cv::resize(keyframe,destination,original_size_,0,0,cv::INTER_LINEAR);
        previous_render_=destination;
        return;
    }
    const PatchData&p=patch.back();if(p.keyframe_id!=keyframe_id_||p.original_size!=original_size_||p.grid_x==0||p.grid_y==0||p.mesh.size()!=static_cast<std::size_t>(p.grid_x)*p.grid_y){destination.release();return;}
    cv::Mat grid(p.grid_y,p.grid_x,CV_32FC2);for(int y=0;y<p.grid_y;++y){cv::Vec2f*row=grid.ptr<cv::Vec2f>(y);for(int x=0;x<p.grid_x;++x){const cv::Point2f v=p.mesh[y*p.grid_x+x];row[x]=cv::Vec2f(v.x,v.y);}}
    cv::resize(grid,dense_mesh_,original_size_,0,0,cv::INTER_CUBIC);

    const float a00=p.affine[0],a01=p.affine[1],a02=p.affine[2],a10=p.affine[3],a11=p.affine[4],a12=p.affine[5];
    const float source_scale_x=(original_size_.width>1&&keyframe.cols>1)?static_cast<float>(keyframe.cols-1)/static_cast<float>(original_size_.width-1):1.0f;
    const float source_scale_y=(original_size_.height>1&&keyframe.rows>1)?static_cast<float>(keyframe.rows-1)/static_cast<float>(original_size_.height-1):1.0f;
    map_x_.create(original_size_,CV_32F);map_y_.create(original_size_,CV_32F);valid_mask_.create(original_size_,CV_8U);valid_mask_.setTo(cv::Scalar(0));

    if (p.homography) {
        const cv::Matx33f h(a00,a01,a02,a10,a11,a12,p.perspective[0],p.perspective[1],1.0f);
        bool invertible=false;
        const cv::Matx33f inv=h.inv(cv::DECOMP_LU,&invertible);
        if(!invertible){destination.release();return;}
        cv::parallel_for_(cv::Range(0,original_size_.height),[&](const cv::Range&r){for(int y=r.start;y<r.end;++y){const cv::Vec2f*dr=dense_mesh_.ptr<cv::Vec2f>(y);float*mx=map_x_.ptr<float>(y);float*my=map_y_.ptr<float>(y);unsigned char*vm=valid_mask_.ptr<unsigned char>(y);for(int x=0;x<original_size_.width;++x){const float tx=x-dr[x][0],ty=y-dr[x][1],d=inv(2,0)*tx+inv(2,1)*ty+inv(2,2);if(std::abs(d)<1e-8f){mx[x]=-1.0f;my[x]=-1.0f;continue;}const float sx=(inv(0,0)*tx+inv(0,1)*ty+inv(0,2))/d,sy=(inv(1,0)*tx+inv(1,1)*ty+inv(1,2))/d;const float kx=sx*source_scale_x,ky=sy*source_scale_y;mx[x]=kx;my[x]=ky;if(kx>=0&&kx<keyframe.cols-1&&ky>=0&&ky<keyframe.rows-1)vm[x]=255;}}});
    } else {
        const float det=a00*a11-a01*a10;if(std::abs(det)<1e-9f){destination.release();return;}
        const float i00=a11/det,i01=-a01/det,i10=-a10/det,i11=a00/det,i02=-(i00*a02+i01*a12),i12=-(i10*a02+i11*a12);
        cv::parallel_for_(cv::Range(0,original_size_.height),[&](const cv::Range&r){for(int y=r.start;y<r.end;++y){const cv::Vec2f*dr=dense_mesh_.ptr<cv::Vec2f>(y);float*mx=map_x_.ptr<float>(y);float*my=map_y_.ptr<float>(y);unsigned char*vm=valid_mask_.ptr<unsigned char>(y);for(int x=0;x<original_size_.width;++x){const float tx=x-dr[x][0],ty=y-dr[x][1],sx=i00*tx+i01*ty+i02,sy=i10*tx+i11*ty+i12;const float kx=sx*source_scale_x,ky=sy*source_scale_y;mx[x]=kx;my[x]=ky;if(kx>=0&&kx<keyframe.cols-1&&ky>=0&&ky<keyframe.rows-1)vm[x]=255;}}});
    }

    if(reuse_previous_frame_borders_){
        if(!previous_render_.empty()&&previous_render_.size()==original_size_&&previous_render_.type()==keyframe.type())previous_render_.copyTo(destination);
        else if(keyframe.size()==original_size_)keyframe.copyTo(destination);
        else cv::resize(keyframe,destination,original_size_,0,0,cv::INTER_LINEAR);
        cv::remap(keyframe,destination,map_x_,map_y_,cv::INTER_LINEAR,cv::BORDER_TRANSPARENT);
    }else{
        cv::remap(keyframe,destination,map_x_,map_y_,cv::INTER_LINEAR,cv::BORDER_CONSTANT,cv::Scalar(0));
        destination=fillOutside(destination,valid_mask_);
    }
    previous_render_=destination;
}

} // namespace affinecodec

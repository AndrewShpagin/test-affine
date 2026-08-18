#include "udp_image_codec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace affinecodec {
namespace motion_refine_detail {

constexpr double kHomographyHuberPixels = 3.0;
constexpr double kHomographyMaxDenominatorVariation = 0.08;
constexpr int kHomographyIterations = 5;
constexpr double kTransformOutlierMeanMultiplier = 3.0;

using Homography8 = std::array<double, 8>;

static double huberWeight(double error_pixels) {
    if (error_pixels <= kHomographyHuberPixels || error_pixels <= 1e-12) return 1.0;
    return kHomographyHuberPixels / error_pixels;
}

static double pointErrorPixels(const Homography8& h,
                               const cv::Point2f& p0,
                               const cv::Point2f& p1,
                               double cx, double cy, double scale) {
    const double x = (p0.x - cx) / scale;
    const double y = (p0.y - cy) / scale;
    const double uo = (p1.x - cx) / scale;
    const double vo = (p1.y - cy) / scale;
    const double d = h[6] * x + h[7] * y + 1.0;
    if (std::abs(d) < 0.25) return std::numeric_limits<double>::infinity();
    const double u = (h[0] * x + h[1] * y + h[2]) / d;
    const double v = (h[3] * x + h[4] * y + h[5]) / d;
    const double dx = (uo - u) * scale;
    const double dy = (vo - v) * scale;
    return std::sqrt(dx * dx + dy * dy);
}

static double homographyCost(const Homography8& h,
                             const std::vector<cv::Point2f>& p0,
                             const std::vector<cv::Point2f>& p1,
                             double cx, double cy, double scale) {
    if (p0.size() != p1.size() || p0.empty()) return std::numeric_limits<double>::infinity();
    double cost = 0.0;
    for (std::size_t i = 0; i < p0.size(); ++i) {
        const double e = pointErrorPixels(h, p0[i], p1[i], cx, cy, scale);
        if (!std::isfinite(e)) return std::numeric_limits<double>::infinity();
        cost += e <= kHomographyHuberPixels
            ? 0.5 * e * e
            : kHomographyHuberPixels * (e - 0.5 * kHomographyHuberPixels);
    }
    return cost / static_cast<double>(p0.size());
}

static void clampPerspective(Homography8& h, const cv::Size& size, double scale) {
    const double ex = 0.5 * std::max(0, size.width - 1) / scale;
    const double ey = 0.5 * std::max(0, size.height - 1) / scale;
    const double variation = std::abs(h[6]) * ex + std::abs(h[7]) * ey;
    if (variation > kHomographyMaxDenominatorVariation && variation > 0.0) {
        const double f = kHomographyMaxDenominatorVariation / variation;
        h[6] *= f;
        h[7] *= f;
    }
}

static Homography8 affineSeed(const cv::Mat& affine,
                              double cx, double cy, double scale) {
    return {
        affine.at<double>(0,0), affine.at<double>(0,1),
        (affine.at<double>(0,0) * cx + affine.at<double>(0,1) * cy + affine.at<double>(0,2) - cx) / scale,
        affine.at<double>(1,0), affine.at<double>(1,1),
        (affine.at<double>(1,0) * cx + affine.at<double>(1,1) * cy + affine.at<double>(1,2) - cy) / scale,
        0.0, 0.0
    };
}

static bool refineHomography(const std::vector<cv::Point2f>& p0,
                             const std::vector<cv::Point2f>& p1,
                             const cv::Size& size,
                             double cx, double cy, double scale,
                             const Homography8& seed,
                             Homography8& result) {
    if (p0.size() != p1.size() || p0.size() < 8) return false;

    Homography8 h = seed;
    double best_cost = homographyCost(h, p0, p1, cx, cy, scale);
    if (!std::isfinite(best_cost)) return false;

    // Keep the solution near its seed. The second pass is seeded by the first-pass
    // homography, so after outlier rejection this prior preserves temporal/geometric stability.
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
            const double weight = huberWeight(err_px);

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
            atb.at<double>(k, 0) += prior_weight[k] * (seed[k] - h[k]);
        }

        cv::Mat delta;
        if (!cv::solve(ata, atb, delta, cv::DECOMP_CHOLESKY) &&
            !cv::solve(ata, atb, delta, cv::DECOMP_SVD)) break;

        bool accepted = false;
        double accepted_step = 0.0;
        for (double step : {1.0, 0.5, 0.25, 0.125}) {
            Homography8 candidate = h;
            for (int k = 0; k < 8; ++k)
                candidate[k] += step * delta.at<double>(k, 0);
            clampPerspective(candidate, size, scale);
            const double cost = homographyCost(candidate, p0, p1, cx, cy, scale);
            if (std::isfinite(cost) && cost < best_cost) {
                h = candidate;
                best_cost = cost;
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

    result = h;
    return true;
}

static bool toPixelHomography(const Homography8& h,
                              const cv::Size& size,
                              double cx, double cy, double scale,
                              std::array<float, 6>& numerator,
                              std::array<float, 2>& perspective) {
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

} // namespace motion_refine_detail

bool refineHomographyFromAffine(std::vector<cv::Point2f>& p0,
                                std::vector<cv::Point2f>& p1,
                                const cv::Mat& affine,
                                const cv::Size& size,
                                std::array<float, 6>& numerator,
                                std::array<float, 2>& perspective) {
    using namespace motion_refine_detail;
    if (p0.size() != p1.size() || p0.size() < 8 || affine.empty()) return false;

    cv::Mat affine64;
    if (affine.type() == CV_64F) affine64 = affine;
    else affine.convertTo(affine64, CV_64F);

    const double scale = std::max(1.0, static_cast<double>(std::max(size.width, size.height)));
    const double cx = 0.5 * std::max(0, size.width - 1);
    const double cy = 0.5 * std::max(0, size.height - 1);

    const Homography8 affine_seed = affineSeed(affine64, cx, cy, scale);
    Homography8 first_h;
    if (!refineHomography(p0, p1, size, cx, cy, scale, affine_seed, first_h)) return false;

    double error_sum = 0.0;
    std::vector<double> errors(p0.size());
    for (std::size_t i = 0; i < p0.size(); ++i) {
        errors[i] = pointErrorPixels(first_h, p0[i], p1[i], cx, cy, scale);
        if (!std::isfinite(errors[i])) return false;
        error_sum += errors[i];
    }

    const double mean_error = error_sum / static_cast<double>(errors.size());
    const double reject_threshold = mean_error * kTransformOutlierMeanMultiplier;

    Homography8 final_h = first_h;
    if (mean_error > 1e-9) {
        std::vector<cv::Point2f> filtered0;
        std::vector<cv::Point2f> filtered1;
        filtered0.reserve(p0.size());
        filtered1.reserve(p1.size());
        for (std::size_t i = 0; i < p0.size(); ++i) {
            if (errors[i] <= reject_threshold) {
                filtered0.push_back(p0[i]);
                filtered1.push_back(p1[i]);
            }
        }

        if (filtered0.size() >= 8 && filtered0.size() < p0.size()) {
            Homography8 refit_h;
            if (refineHomography(filtered0, filtered1, size, cx, cy, scale, first_h, refit_h)) {
                final_h = refit_h;
                p0.swap(filtered0);
                p1.swap(filtered1);
            }
        }
    }

    return toPixelHomography(final_h, size, cx, cy, scale, numerator, perspective);
}

} // namespace affinecodec

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct TrackSet {
    std::vector<cv::Point2f> p0;
    std::vector<cv::Point2f> p1;
};

struct FitResult {
    cv::Mat M;
    cv::Mat inliers;
    double inlierRatio = 0.0;
    double mae = 0.0;
    double rmse = 0.0;
    double validFraction = 0.0;
};

static bool isImageFile(const fs::path& p) {
    if (!p.has_extension()) return false;
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".bmp" || e == ".tif" || e == ".tiff";
}

static std::vector<fs::path> listImages(const fs::path& dir) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.is_regular_file() && isImageFile(e.path())) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

static TrackSet trackLK(const cv::Mat& a, const cv::Mat& b) {
    constexpr int MAX_CORNERS = 600;
    constexpr double QUALITY = 0.01;
    constexpr double MIN_DISTANCE = 8.0;
    constexpr int BLOCK_SIZE = 7;
    constexpr float FB_MAX = 1.5f;
    constexpr float LK_ERR_MAX = 30.0f;

    std::vector<cv::Point2f> p0;
    cv::goodFeaturesToTrack(a, p0, MAX_CORNERS, QUALITY, MIN_DISTANCE,
                            cv::noArray(), BLOCK_SIZE, false, 0.04);

    TrackSet out;
    if (p0.empty()) return out;

    std::vector<cv::Point2f> p1;
    std::vector<uchar> st01;
    std::vector<float> err01;
    cv::calcOpticalFlowPyrLK(
        a, b, p0, p1, st01, err01,
        cv::Size(21, 21), 4,
        cv::TermCriteria(cv::TermCriteria::COUNT | cv::TermCriteria::EPS, 30, 0.01),
        0, 1e-4);

    std::vector<cv::Point2f> good0, good1;
    good0.reserve(p0.size());
    good1.reserve(p0.size());
    for (size_t i = 0; i < p0.size(); ++i) {
        if (st01[i] && err01[i] < LK_ERR_MAX) {
            good0.push_back(p0[i]);
            good1.push_back(p1[i]);
        }
    }
    if (good0.empty()) return out;

    std::vector<cv::Point2f> back;
    std::vector<uchar> st10;
    std::vector<float> err10;
    cv::calcOpticalFlowPyrLK(
        b, a, good1, back, st10, err10,
        cv::Size(21, 21), 4,
        cv::TermCriteria(cv::TermCriteria::COUNT | cv::TermCriteria::EPS, 30, 0.01));

    out.p0.reserve(good0.size());
    out.p1.reserve(good0.size());
    for (size_t i = 0; i < good0.size(); ++i) {
        if (!st10[i]) continue;
        if (cv::norm(back[i] - good0[i]) > FB_MAX) continue;
        out.p0.push_back(good0[i]);
        out.p1.push_back(good1[i]);
    }
    return out;
}

static void calcResidual(const cv::Mat& a, const cv::Mat& b, const cv::Mat& M,
                         double& mae, double& rmse, double& validFraction,
                         cv::Mat* warpedOut = nullptr, cv::Mat* diffOut = nullptr) {
    cv::Mat warped;
    cv::warpAffine(a, warped, M, b.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, 0);

    cv::Mat srcMask(a.size(), CV_8U, cv::Scalar(255));
    cv::Mat mask;
    cv::warpAffine(srcMask, mask, M, b.size(), cv::INTER_NEAREST, cv::BORDER_CONSTANT, 0);
    cv::erode(mask, mask, cv::Mat::ones(5, 5, CV_8U));

    cv::Mat diff;
    cv::absdiff(warped, b, diff);

    const cv::Scalar meanAbs = cv::mean(diff, mask);
    mae = meanAbs[0];

    cv::Mat diffF;
    diff.convertTo(diffF, CV_32F);
    cv::multiply(diffF, diffF, diffF);
    rmse = std::sqrt(cv::mean(diffF, mask)[0]);
    validFraction = double(cv::countNonZero(mask)) / double(mask.total());

    if (warpedOut) *warpedOut = warped;
    if (diffOut) *diffOut = diff;
}

static FitResult fitAffine(const cv::Mat& a, const cv::Mat& b,
                           const TrackSet& t, bool fullAffine) {
    FitResult r;
    if (t.p0.size() < 6) return r;

    if (fullAffine) {
        r.M = cv::estimateAffine2D(t.p0, t.p1, r.inliers,
                                   cv::RANSAC, 2.0, 3000, 0.995, 10);
    } else {
        r.M = cv::estimateAffinePartial2D(t.p0, t.p1, r.inliers,
                                          cv::RANSAC, 2.0, 3000, 0.995, 10);
    }
    if (r.M.empty()) return r;

    r.inlierRatio = double(cv::countNonZero(r.inliers)) / double(r.inliers.total());
    calcResidual(a, b, r.M, r.mae, r.rmse, r.validFraction);
    return r;
}

static cv::Mat drawTracks(const cv::Mat& gray, const TrackSet& t, const cv::Mat& inliers) {
    cv::Mat vis;
    cv::cvtColor(gray, vis, cv::COLOR_GRAY2BGR);
    for (size_t i = 0; i < t.p0.size(); ++i) {
        bool in = !inliers.empty() && inliers.at<uchar>(int(i), 0) != 0;
        const cv::Scalar color = in ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);
        cv::line(vis, t.p0[i], t.p1[i], color, 1, cv::LINE_AA);
        cv::circle(vis, t.p1[i], 2, color, -1, cv::LINE_AA);
    }
    return vis;
}

static void writeDiagnostic(const fs::path& outPath,
                            const cv::Mat& a, const cv::Mat& b,
                            const TrackSet& t, const FitResult& f) {
    if (f.M.empty()) return;

    cv::Mat warped, diff;
    double mae, rmse, vf;
    calcResidual(a, b, f.M, mae, rmse, vf, &warped, &diff);

    cv::Mat diffVis;
    diff.convertTo(diffVis, CV_8U, 3.0);

    cv::Mat tracks = drawTracks(b, t, f.inliers);
    cv::Mat bBgr, warpedBgr, diffBgr;
    cv::cvtColor(b, bBgr, cv::COLOR_GRAY2BGR);
    cv::cvtColor(warped, warpedBgr, cv::COLOR_GRAY2BGR);
    cv::cvtColor(diffVis, diffBgr, cv::COLOR_GRAY2BGR);

    cv::Mat top, bottom, canvas;
    cv::hconcat(std::vector<cv::Mat>{bBgr, warpedBgr}, top);
    cv::hconcat(std::vector<cv::Mat>{tracks, diffBgr}, bottom);
    cv::vconcat(std::vector<cv::Mat>{top, bottom}, canvas);

    const std::string text = cv::format("inliers %.1f%%   MAE %.2f   RMSE %.2f",
                                        100.0 * f.inlierRatio, f.mae, f.rmse);
    cv::putText(canvas, text, cv::Point(10, 24), cv::FONT_HERSHEY_SIMPLEX,
                0.55, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
    cv::imwrite(outPath.string(), canvas);
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: affine_test <image-folder> [output-folder]\n";
        return 2;
    }

    const fs::path inputDir = argv[1];
    const fs::path outputDir = argc >= 3 ? fs::path(argv[2]) : fs::path("affine-out");
    fs::create_directories(outputDir);

    const auto files = listImages(inputDir);
    if (files.size() < 2) {
        std::cerr << "Need at least two images in " << inputDir << "\n";
        return 1;
    }

    std::ofstream csv(outputDir / "metrics.csv");
    csv << "pair,file0,file1,lk_points,partial_inlier,partial_mae,partial_rmse,"
           "full_inlier,full_mae,full_rmse,a00,a01,tx,a10,a11,ty\n";

    double sumPartial = 0.0, sumFull = 0.0;
    int countPartial = 0, countFull = 0;

    for (size_t i = 0; i + 1 < files.size(); ++i) {
        cv::Mat a = cv::imread(files[i].string(), cv::IMREAD_GRAYSCALE);
        cv::Mat b = cv::imread(files[i + 1].string(), cv::IMREAD_GRAYSCALE);
        if (a.empty() || b.empty() || a.size() != b.size()) {
            std::cerr << "Skipping incompatible pair: " << files[i] << " / " << files[i + 1] << "\n";
            continue;
        }

        TrackSet t = trackLK(a, b);
        FitResult partial = fitAffine(a, b, t, false);
        FitResult full = fitAffine(a, b, t, true);

        if (!partial.M.empty()) { sumPartial += partial.mae; ++countPartial; }
        if (!full.M.empty()) { sumFull += full.mae; ++countFull; }

        std::cout << std::setw(3) << i
                  << "  LK=" << std::setw(3) << t.p0.size()
                  << "  partial: " << std::fixed << std::setprecision(1)
                  << (partial.M.empty() ? 0.0 : 100.0 * partial.inlierRatio) << "% MAE="
                  << (partial.M.empty() ? 0.0 : partial.mae)
                  << "  full: " << (full.M.empty() ? 0.0 : 100.0 * full.inlierRatio)
                  << "% MAE=" << (full.M.empty() ? 0.0 : full.mae) << "\n";

        csv << i << ',' << files[i].filename().string() << ',' << files[i + 1].filename().string()
            << ',' << t.p0.size() << ',';
        if (!partial.M.empty()) csv << partial.inlierRatio << ',' << partial.mae << ',' << partial.rmse;
        csv << ',';
        if (!full.M.empty()) {
            csv << full.inlierRatio << ',' << full.mae << ',' << full.rmse << ','
                << full.M.at<double>(0,0) << ',' << full.M.at<double>(0,1) << ',' << full.M.at<double>(0,2) << ','
                << full.M.at<double>(1,0) << ',' << full.M.at<double>(1,1) << ',' << full.M.at<double>(1,2);
        }
        csv << '\n';

        char name[64];
        std::snprintf(name, sizeof(name), "%04zu.jpg", i);
        writeDiagnostic(outputDir / name, a, b, t, full);
    }

    std::cout << "\nMean MAE: partial="
              << (countPartial ? sumPartial / countPartial : 0.0)
              << " full=" << (countFull ? sumFull / countFull : 0.0) << "\n";
    std::cout << "Diagnostics: " << outputDir << "\n";
    return 0;
}

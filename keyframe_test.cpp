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

struct TrackSet { std::vector<cv::Point2f> p0, p1; };

static bool isImageFile(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c){ return char(std::tolower(c)); });
    return e==".jpg" || e==".jpeg" || e==".png" || e==".bmp" || e==".tif" || e==".tiff";
}

static std::vector<fs::path> listImages(const fs::path& dir) {
    std::vector<fs::path> v;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.is_regular_file() && isImageFile(e.path())) v.push_back(e.path());
    std::sort(v.begin(), v.end());
    return v;
}

static std::vector<cv::Point2f> selectGridFeatures(const cv::Mat& image,
                                                    int gridX = 8,
                                                    int gridY = 8,
                                                    int pointsPerCell = 3) {
    std::vector<cv::Point2f> points;
    points.reserve(gridX * gridY * pointsPerCell);

    for (int gy = 0; gy < gridY; ++gy) {
        const int y0 = gy * image.rows / gridY;
        const int y1 = (gy + 1) * image.rows / gridY;
        for (int gx = 0; gx < gridX; ++gx) {
            const int x0 = gx * image.cols / gridX;
            const int x1 = (gx + 1) * image.cols / gridX;
            const cv::Rect roi(x0, y0, x1 - x0, y1 - y0);
            if (roi.width < 7 || roi.height < 7) continue;

            std::vector<cv::Point2f> local;
            cv::goodFeaturesToTrack(image(roi), local, pointsPerCell,
                                    0.01, 7.0, cv::noArray(), 7, false, 0.04);
            for (cv::Point2f p : local) {
                p.x += float(x0);
                p.y += float(y0);
                points.push_back(p);
            }
        }
    }
    return points;
}

static TrackSet trackLK(const cv::Mat& a, const cv::Mat& b) {
    std::vector<cv::Point2f> p0 = selectGridFeatures(a, 8, 8, 3);
    TrackSet out;
    if (p0.empty()) return out;

    std::vector<cv::Point2f> p1;
    std::vector<uchar> st01;
    std::vector<float> err01;
    cv::calcOpticalFlowPyrLK(a, b, p0, p1, st01, err01, cv::Size(21,21), 5,
        cv::TermCriteria(cv::TermCriteria::COUNT | cv::TermCriteria::EPS, 40, 0.01), 0, 1e-4);

    std::vector<cv::Point2f> g0, g1;
    for (size_t i=0; i<p0.size(); ++i)
        if (st01[i] && err01[i] < 35.f) { g0.push_back(p0[i]); g1.push_back(p1[i]); }
    if (g0.empty()) return out;

    std::vector<cv::Point2f> back;
    std::vector<uchar> st10;
    std::vector<float> err10;
    cv::calcOpticalFlowPyrLK(b, a, g1, back, st10, err10, cv::Size(21,21), 5,
        cv::TermCriteria(cv::TermCriteria::COUNT | cv::TermCriteria::EPS, 40, 0.01));

    for (size_t i=0; i<g0.size(); ++i) {
        if (!st10[i]) continue;
        if (cv::norm(back[i] - g0[i]) > 1.5) continue;
        out.p0.push_back(g0[i]);
        out.p1.push_back(g1[i]);
    }
    return out;
}

static cv::Point2f applyAffine(const cv::Mat& M, const cv::Point2f& p) {
    return {
        float(M.at<double>(0,0)*p.x + M.at<double>(0,1)*p.y + M.at<double>(0,2)),
        float(M.at<double>(1,0)*p.x + M.at<double>(1,1)*p.y + M.at<double>(1,2))
    };
}

static double maskedMAE(const cv::Mat& a, const cv::Mat& b, const cv::Mat& mask) {
    cv::Mat diff;
    cv::absdiff(a, b, diff);
    return cv::mean(diff, mask)[0];
}

struct WarpResult {
    cv::Mat affineWarp;
    cv::Mat meshWarp;
    cv::Mat affineValid;
    cv::Mat meshValid;
    double affineMae = 0.0;
    double meshMae = 0.0;
    int tracks = 0;
    double inlierRatio = 0.0;
    bool ok = false;
};

static WarpResult warpFromReference(const cv::Mat& ref, const cv::Mat& cur,
                                    int gx=4, int gy=4, double sigma=80.0) {
    WarpResult r;
    TrackSet t = trackLK(ref, cur);
    r.tracks = int(t.p0.size());
    if (t.p0.size() < 4) return r;

    cv::Mat inliers;
    cv::Mat affine = cv::estimateAffinePartial2D(t.p0, t.p1, inliers,
        cv::RANSAC, 2.0, 3000, 0.995, 10);
    if (affine.empty()) return r;
    r.inlierRatio = double(cv::countNonZero(inliers)) / double(inliers.total());

    cv::warpAffine(ref, r.affineWarp, affine, cur.size(),
                   cv::INTER_LINEAR, cv::BORDER_CONSTANT, 0);

    cv::Mat srcMask(ref.size(), CV_8U, cv::Scalar(255));
    cv::warpAffine(srcMask, r.affineValid, affine, cur.size(),
                   cv::INTER_NEAREST, cv::BORDER_CONSTANT, 0);

    cv::Mat affineMetricValid = r.affineValid.clone();
    cv::erode(affineMetricValid, affineMetricValid, cv::Mat::ones(5,5,CV_8U));

    std::vector<cv::Point2f> q, residual;
    for (size_t i=0; i<t.p0.size(); ++i) {
        if (!inliers.empty() && inliers.at<uchar>(int(i), 0) == 0) continue;
        cv::Point2f qi = applyAffine(affine, t.p0[i]);
        cv::Point2f ri = t.p1[i] - qi;
        if (cv::norm(ri) < 10.0) {
            q.push_back(qi);
            residual.push_back(ri);
        }
    }

    cv::Mat grid(gy, gx, CV_32FC2, cv::Scalar(0,0));
    for (int iy=0; iy<gy; ++iy) {
        const float y = gy==1 ? 0.f : float(iy)*(cur.rows-1)/float(gy-1);
        for (int ix=0; ix<gx; ++ix) {
            const float x = gx==1 ? 0.f : float(ix)*(cur.cols-1)/float(gx-1);
            cv::Point2d s(0,0);
            double sw = 0.0;
            for (size_t k=0; k<q.size(); ++k) {
                const double dx=q[k].x-x, dy=q[k].y-y;
                const double w=std::exp(-(dx*dx+dy*dy)/(2.0*sigma*sigma));
                s.x += w*residual[k].x;
                s.y += w*residual[k].y;
                sw += w;
            }
            if (sw > 1e-9)
                grid.at<cv::Vec2f>(iy,ix) = cv::Vec2f(float(s.x/sw), float(s.y/sw));
        }
    }

    cv::Mat dense;
    cv::resize(grid, dense, cur.size(), 0, 0, cv::INTER_CUBIC);

    cv::Mat inv;
    cv::invertAffineTransform(affine, inv);
    cv::Mat mapx(cur.size(), CV_32F), mapy(cur.size(), CV_32F);
    r.meshValid = cv::Mat(cur.size(), CV_8U, cv::Scalar(0));
    cv::Mat meshMetricValid(cur.size(), CV_8U, cv::Scalar(0));

    for (int y=0; y<cur.rows; ++y) {
        const cv::Vec2f* dr=dense.ptr<cv::Vec2f>(y);
        float* mx=mapx.ptr<float>(y);
        float* my=mapy.ptr<float>(y);
        uchar* vm=r.meshValid.ptr<uchar>(y);
        uchar* mm=meshMetricValid.ptr<uchar>(y);
        for (int x=0; x<cur.cols; ++x) {
            const double tx=x-dr[x][0], ty=y-dr[x][1];
            const double sx=inv.at<double>(0,0)*tx + inv.at<double>(0,1)*ty + inv.at<double>(0,2);
            const double sy=inv.at<double>(1,0)*tx + inv.at<double>(1,1)*ty + inv.at<double>(1,2);
            mx[x]=float(sx);
            my[x]=float(sy);
            if (sx>=0 && sx<ref.cols-1 && sy>=0 && sy<ref.rows-1) vm[x]=255;
            if (sx>=2 && sx<ref.cols-2 && sy>=2 && sy<ref.rows-2) mm[x]=255;
        }
    }

    cv::remap(ref, r.meshWarp, mapx, mapy,
              cv::INTER_LINEAR, cv::BORDER_CONSTANT, 0);

    cv::Mat commonValid;
    cv::bitwise_and(affineMetricValid, meshMetricValid, commonValid);
    if (cv::countNonZero(commonValid) == 0) return r;

    r.affineMae = maskedMAE(r.affineWarp, cur, commonValid);
    r.meshMae = maskedMAE(r.meshWarp, cur, commonValid);
    r.ok = true;
    return r;
}

static cv::Mat fillOutsideByPropagation(const cv::Mat& gray, const cv::Mat& valid) {
    CV_Assert(gray.type() == CV_8U && valid.type() == CV_8U && gray.size() == valid.size());
    cv::Mat out = gray.clone();
    if (cv::countNonZero(valid) == 0) return out;

    cv::Mat state(gray.size(), CV_8U, cv::Scalar(0));
    for (int y = 0; y < gray.rows; ++y) {
        const uchar* vm = valid.ptr<uchar>(y);
        uchar* sm = state.ptr<uchar>(y);
        for (int x = 0; x < gray.cols; ++x)
            if (vm[x]) sm[x] = 2;
    }

    static const int dx[8] = {-1,0,1,-1,1,-1,0,1};
    static const int dy[8] = {-1,-1,-1,0,0,1,1,1};

    std::vector<cv::Point> queue;
    queue.reserve(gray.total());

    for (int y = 0; y < gray.rows; ++y) {
        for (int x = 0; x < gray.cols; ++x) {
            if (state.at<uchar>(y,x) != 0) continue;
            for (int k = 0; k < 8; ++k) {
                const int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= gray.cols || ny >= gray.rows) continue;
                if (state.at<uchar>(ny,nx) == 2) {
                    state.at<uchar>(y,x) = 1;
                    queue.emplace_back(x,y);
                    break;
                }
            }
        }
    }

    size_t head = 0;
    while (head < queue.size()) {
        const cv::Point p = queue[head++];

        int sum = 0, cnt = 0;
        for (int k = 0; k < 8; ++k) {
            const int nx = p.x + dx[k], ny = p.y + dy[k];
            if (nx < 0 || ny < 0 || nx >= gray.cols || ny >= gray.rows) continue;
            if (state.at<uchar>(ny,nx) == 2) {
                sum += out.at<uchar>(ny,nx);
                ++cnt;
            }
        }
        if (cnt > 0)
            out.at<uchar>(p.y,p.x) = uchar((sum + cnt/2) / cnt);
        state.at<uchar>(p.y,p.x) = 2;

        for (int k = 0; k < 8; ++k) {
            const int nx = p.x + dx[k], ny = p.y + dy[k];
            if (nx < 0 || ny < 0 || nx >= gray.cols || ny >= gray.rows) continue;
            uchar& st = state.at<uchar>(ny,nx);
            if (st == 0) {
                st = 1;
                queue.emplace_back(nx,ny);
            }
        }
    }

    cv::Mat prev = out.clone();
    for (int pass = 0; pass < 2; ++pass) {
        out.copyTo(prev);
        for (int y = 0; y < gray.rows; ++y) {
            for (int x = 0; x < gray.cols; ++x) {
                if (valid.at<uchar>(y,x)) continue;
                int sum = 0, cnt = 0;
                for (int k = 0; k < 8; ++k) {
                    const int nx = x + dx[k], ny = y + dy[k];
                    if (nx < 0 || ny < 0 || nx >= gray.cols || ny >= gray.rows) continue;
                    sum += prev.at<uchar>(ny,nx);
                    ++cnt;
                }
                if (cnt > 0) out.at<uchar>(y,x) = uchar((sum + cnt/2) / cnt);
            }
        }
    }

    return out;
}

static cv::Mat labelImage(const cv::Mat& gray,
                          const std::string& title,
                          const std::string& line2) {
    cv::Mat color;
    cv::cvtColor(gray, color, cv::COLOR_GRAY2BGR);
    cv::rectangle(color, cv::Rect(0,0,color.cols,52), cv::Scalar(0,0,0), cv::FILLED);
    cv::putText(color, title, {10,21}, cv::FONT_HERSHEY_SIMPLEX,
                0.55, {255,255,255}, 1, cv::LINE_AA);
    cv::putText(color, line2, {10,43}, cv::FONT_HERSHEY_SIMPLEX,
                0.43, {255,255,255}, 1, cv::LINE_AA);
    return color;
}

int main(int argc, char** argv) {
    if (argc > 6) {
        std::cerr << "Usage: keyframe_test [image-folder] [output-folder] [key-period] [grid-x] [grid-y]\n";
        return 2;
    }

    const fs::path inputDir = argc>1 ? fs::path(argv[1]) : fs::path("local-data/frames");
    const fs::path outputDir = argc>2 ? fs::path(argv[2]) : fs::path("output/keyframe");
    const int N = argc>3 ? std::stoi(argv[3]) : 5;
    const int gx = argc>4 ? std::stoi(argv[4]) : 4;
    const int gy = argc>5 ? std::stoi(argv[5]) : 4;

    if (N < 1) { std::cerr << "key-period must be >= 1\n"; return 2; }
    if (!fs::exists(inputDir)) {
        std::cerr << "Input folder does not exist: " << inputDir << "\n";
        return 1;
    }

    const auto files = listImages(inputDir);
    if (files.size() < 2) {
        std::cerr << "Need at least two images in " << inputDir << "\n";
        return 1;
    }
    fs::create_directories(outputDir);

    std::ofstream csv(outputDir / "keyframe_metrics.csv");
    csv << "frame,keyframe,distance,tracks,inlier_ratio,affine_mae,mesh_mae,gain_percent,ok\n";

    cv::Mat ref;
    size_t keyIndex = 0;

    for (size_t i=0; i<files.size(); ++i) {
        cv::Mat cur = cv::imread(files[i].string(), cv::IMREAD_GRAYSCALE);
        if (cur.empty()) continue;

        if (i % size_t(N) == 0 || ref.empty()) {
            ref = cur.clone();
            keyIndex = i;
        }

        const int distance = int(i - keyIndex);
        cv::Mat affineWarp, meshWarp;
        double affineMae = 0.0;
        double meshMae = 0.0;
        double gain = 0.0;
        int tracks = 0;
        double inlierRatio = 1.0;
        bool ok = true;

        if (i == keyIndex) {
            affineWarp = ref.clone();
            meshWarp = ref.clone();
        } else {
            WarpResult wr = warpFromReference(ref, cur, gx, gy);
            ok = wr.ok;
            tracks = wr.tracks;
            inlierRatio = wr.inlierRatio;
            affineMae = wr.affineMae;
            meshMae = wr.meshMae;
            if (wr.ok) {
                affineWarp = fillOutsideByPropagation(wr.affineWarp, wr.affineValid);
                meshWarp = fillOutsideByPropagation(wr.meshWarp, wr.meshValid);
                if (affineMae > 1e-9)
                    gain = 100.0 * (affineMae - meshMae) / affineMae;
            } else {
                affineWarp = cv::Mat(cur.size(), cur.type(), cv::Scalar(0));
                meshWarp = affineWarp.clone();
            }
        }

        const std::string originalInfo =
            "frame " + std::to_string(i) + "  file " + files[i].filename().string();
        const std::string affineInfo =
            "key " + std::to_string(keyIndex) + " d=" + std::to_string(distance) +
            "  MAE=" + cv::format("%.2f", affineMae) +
            "  inl=" + cv::format("%.0f%%", 100.0*inlierRatio);
        const std::string meshInfo =
            cv::format("mesh %dx%d  MAE=%.2f  gain=%+.1f%%", gx, gy, meshMae, gain);

        cv::Mat originalPanel = labelImage(cur, "ORIGINAL", originalInfo);
        cv::Mat affinePanel = labelImage(
            affineWarp,
            i==keyIndex ? "REFERENCE / KEYFRAME" : "PARTIAL AFFINE ONLY (filled)",
            affineInfo);
        cv::Mat meshPanel = labelImage(
            meshWarp,
            i==keyIndex ? "REFERENCE / KEYFRAME" : "PARTIAL AFFINE + MESH (filled)",
            meshInfo);

        cv::Mat side;
        cv::hconcat(std::vector<cv::Mat>{originalPanel, affinePanel, meshPanel}, side);

        char name[64];
        std::snprintf(name, sizeof(name), "%04zu.jpg", i);
        cv::imwrite((outputDir / name).string(), side, {cv::IMWRITE_JPEG_QUALITY, 95});

        csv << i << ',' << keyIndex << ',' << distance << ',' << tracks << ','
            << inlierRatio << ',' << affineMae << ',' << meshMae << ','
            << gain << ',' << (ok ? 1 : 0) << '\n';

        std::cout << std::setw(4) << i
                  << " key=" << std::setw(4) << keyIndex
                  << " d=" << distance
                  << " tracks=" << tracks
                  << " inliers=" << std::fixed << std::setprecision(1)
                  << 100.0*inlierRatio << "%"
                  << " affine=" << std::setprecision(2) << affineMae
                  << " mesh=" << meshMae
                  << " gain=" << std::setprecision(1) << gain << "%"
                  << (i==keyIndex ? "  KEY" : "") << '\n';
    }

    std::cout << "\nOutput: " << outputDir << "\n"
              << "Every " << N << "th frame is a new reference.\n"
              << "LK features: 8x8 spatial grid, up to 3 Shi-Tomasi corners per cell.\n"
              << "Each output image is ORIGINAL | PARTIAL AFFINE ONLY | PARTIAL AFFINE + MESH.\n"
              << "Visual previews use iterative filled borders; metrics remain based on valid pixels only.\n";
    return 0;
}

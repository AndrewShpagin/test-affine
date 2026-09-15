#include "jpeg_restart_fill.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace affinecodec {
namespace {
double smoothUnit(double x) { x = std::clamp(x, 0.0, 1.0); return x*x*(3-2*x); }
using Faces = std::array<double, 4>;
std::vector<Faces> fillFaces(const std::vector<unsigned char>& valid, int columns, int rows) {
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<Faces> faces(valid.size(), Faces{inf, inf, inf, inf});
    for (int y = 0; y < rows; ++y) {
        int left = -1, right = -1;
        for (int x = 0; x < columns; ++x) {
            const int b = y*columns+x; if (valid[b]) left = x;
            if (left >= 0) faces[b][0] = (x-left-1)*16+1;
        }
        for (int x = columns-1; x >= 0; --x) {
            const int b = y*columns+x; if (valid[b]) right = x;
            if (right >= 0) faces[b][1] = (right-x)*16;
        }
    }
    for (int x = 0; x < columns; ++x) {
        int top = -1, bottom = -1;
        for (int y = 0; y < rows; ++y) {
            const int b = y*columns+x; if (valid[b]) top = y;
            if (top >= 0) faces[b][2] = (y-top-1)*8+1;
        }
        for (int y = rows-1; y >= 0; --y) {
            const int b = y*columns+x; if (valid[b]) bottom = y;
            if (bottom >= 0) faces[b][3] = (bottom-y)*8;
        }
    }
    return faces;
}
}

cv::Mat fillRestartGaps(const cv::Mat& source,
                        const std::vector<unsigned char>& received, bool smooth) {
    const int width = source.cols, height = source.rows, channels = source.channels();
    const int columns = width/16, rows = height/8;
    if (source.empty() || source.depth() != CV_8U || (channels != 1 && channels != 3 && channels != 4) ||
        width%16 || height%8 || received.size() != std::size_t(columns)*rows*2)
        throw std::invalid_argument("bad restart-fill geometry");
    std::vector<unsigned char> valid(columns*rows);
    int known = 0;
    for (std::size_t b = 0; b < valid.size(); ++b) {
        valid[b] = received[2*b] || received[2*b+1]; known += valid[b];
    }
    if (!known || known == int(valid.size())) return source;
    const auto faces = smooth ? fillFaces(valid, columns, rows) : std::vector<Faces>{};
    cv::Mat nearest = source.clone(), controls;
    if (smooth) controls = cv::Mat(height, width, CV_8UC2, cv::Scalar::all(0));
    std::vector<int> nearest_y(columns*height, -1), sites(width);
    std::vector<double> edges(width+1), cost(width);
    const double inf = std::numeric_limits<double>::infinity();
    for (int bx = 0; bx < columns; ++bx) {
        int above = -1, below = -1;
        for (int y = 0; y < height; ++y) {
            if (valid[(y/8)*columns+bx]) above = y;
            nearest_y[y*columns+bx] = above;
        }
        for (int y = height-1; y >= 0; --y) {
            if (valid[(y/8)*columns+bx]) below = y;
            auto& prior = nearest_y[y*columns+bx];
            if (below >= 0 && (prior < 0 || below-y < y-prior)) prior = below;
        }
    }
    // Lower envelope of squared-distance parabolas: exact O(W*H) Euclidean NN.
    for (int y = 0; y < height; ++y) {
        int last = -1;
        for (int x = 0; x < width; ++x) {
            const int sy = nearest_y[y*columns+x/16]; if (sy < 0) continue;
            cost[x] = double(y-sy)*(y-sy);
            double boundary = -inf;
            while (last >= 0) {
                const int p = sites[last];
                boundary = (cost[x]+double(x)*x-cost[p]-double(p)*p)/(2*(x-p));
                if (boundary > edges[last]) break;
                --last;
            }
            ++last; sites[last] = x; edges[last] = last == 0 ? -inf : boundary; edges[last+1] = inf;
        }
        int site = 0;
        for (int x = 0; x < width; ++x) {
            const int b = (y/8)*columns+x/16; if (valid[b]) continue;
            while (site < last && edges[site+1] < x) ++site; // ties: left, then above
            const int sx = sites[site], sy = nearest_y[y*columns+sx/16];
            std::copy_n(source.ptr(sy)+sx*channels, channels, nearest.ptr(y)+x*channels);
            if (!smooth) continue;
            const double t = smoothUnit((std::hypot(x-sx, y-sy)-1)/3);
            double first = inf, second = inf;
            const int offsets[4] = {x%16, -(x%16), y%8, -(y%8)};
            for (int side = 0; side < 4; ++side) {
                const double d = faces[b][side]+offsets[side];
                if (d < first) { second = first; first = d; } else if (d < second) second = d;
            }
            const double seam = std::isfinite(second) ? 1-smoothUnit((second-first-1)/1.5) : 1;
            // Match browser RG8 quantization before applying the kernel.
            controls.at<cv::Vec2b>(y,x) = cv::Vec2b(std::lround(255*t*(.5+.5*seam)), std::lround(255*t));
        }
    }
    if (!smooth) return nearest;
    cv::Mat result = nearest.clone();
    constexpr double q = .7071067811865476;
    constexpr double taps[12][3] = {{-.5,0,2},{.5,0,2},{0,-.5,2},{0,.5,2},
        {-1,0,1},{1,0,1},{0,-1,1},{0,1,1},{-q,-q,1},{-q,q,1},{q,-q,1},{q,q,1}};
    cv::parallel_for_(cv::Range(0,height), [&](const cv::Range& range) {
        for (int y = range.start; y < range.end; ++y) for (int x = 0; x < width; ++x) {
            const auto control = controls.at<cv::Vec2b>(y,x); if (!control[1]) continue;
            const double radius = control[0]*3.0/255, blend = control[1]/255.0;
            const auto* original = nearest.ptr(y)+x*channels;
            double sum[4] = {0,0,0,0};
            const int colors = std::min(channels, 3); // preserve RGBA alpha
            for (int c = 0; c < colors; ++c) sum[c] = original[c]*4.0;
            for (const auto& tap : taps) {
                const double px = std::clamp(x+radius*tap[0], 0.0, double(width-1));
                const double py = std::clamp(y+radius*tap[1], 0.0, double(height-1));
                const int x0 = int(px), y0 = int(py), x1 = std::min(x0+1,width-1), y1 = std::min(y0+1,height-1);
                const double a = px-x0, b = py-y0;
                const auto* top = nearest.ptr(y0); const auto* bottom = nearest.ptr(y1);
                for (int c = 0; c < colors; ++c)
                    sum[c] += tap[2]*((1-b)*((1-a)*top[x0*channels+c]+a*top[x1*channels+c])
                                       +b*((1-a)*bottom[x0*channels+c]+a*bottom[x1*channels+c]));
            }
            for (int c = 0; c < colors; ++c)
                result.ptr(y)[x*channels+c] = static_cast<unsigned char>(std::lround(original[c]*(1-blend)+sum[c]*blend/20));
        }
    });
    return result;
}
} // namespace affinecodec

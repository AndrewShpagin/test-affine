#include "udp_image_codec.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using affinecodec::Decoder;
using affinecodec::Encoder;
using affinecodec::PatchData;
using affinecodec::u_char;

static bool isImageFile(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ".jpg" || e == ".jpeg" || e == ".png" ||
           e == ".bmp" || e == ".tif" || e == ".tiff";
}

static std::vector<fs::path> listImages(const fs::path& dir) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.is_regular_file() && isImageFile(e.path())) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    return files;
}

static double colorMae(const cv::Mat& a, const cv::Mat& b) {
    if (a.empty() || b.empty() || a.size() != b.size() || a.type() != b.type()) return 0.0;
    cv::Mat diff;
    cv::absdiff(a, b, diff);
    const cv::Scalar m = cv::mean(diff);
    double sum = 0.0;
    for (int c = 0; c < a.channels(); ++c) sum += m[c];
    return sum / a.channels();
}

static cv::Mat addLabel(const cv::Mat& image, const std::string& title,
                        const std::string& info) {
    cv::Mat out = image.clone();
    if (out.channels() == 1) cv::cvtColor(out, out, cv::COLOR_GRAY2BGR);
    cv::rectangle(out, cv::Rect(0, 0, out.cols, std::min(52, out.rows)),
                  cv::Scalar(0, 0, 0), cv::FILLED);
    cv::putText(out, title, cv::Point(10, 21), cv::FONT_HERSHEY_SIMPLEX,
                0.55, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    cv::putText(out, info, cv::Point(10, 43), cv::FONT_HERSHEY_SIMPLEX,
                0.43, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    return out;
}

int main(int argc, char** argv) {
    if (argc > 5) {
        std::cerr << "Usage: codec_test [image-folder] [output-folder] [jpeg-bytes] [key-period]\n";
        return 2;
    }

    const fs::path input_dir = argc > 1 ? fs::path(argv[1]) : fs::path("local-data/frames");
    const fs::path output_dir = argc > 2 ? fs::path(argv[2]) : fs::path("output/codec");
    const int jpeg_bytes = argc > 3 ? std::stoi(argv[3]) : 40000;
    const int key_period = argc > 4 ? std::stoi(argv[4]) : 5;

    if (!fs::exists(input_dir)) {
        std::cerr << "Input folder does not exist: " << input_dir << '\n';
        return 1;
    }

    const std::vector<fs::path> files = listImages(input_dir);
    if (files.empty()) {
        std::cerr << "No images in " << input_dir << '\n';
        return 1;
    }
    fs::create_directories(output_dir);

    std::cout << "Input folder: " << input_dir << '\n'
              << "Output folder: " << output_dir << '\n'
              << "Target JPEG bytes: " << jpeg_bytes << '\n'
              << "Keyframe period: " << key_period << '\n'
              << "Max packet bytes: " << affinecodec::kMaxUdpPacketBytes << '\n'
              << "Number of images: " << files.size() << "\n\n";

    Encoder encoder;
    Decoder decoder;
    std::vector<u_char> current_jpeg;
    std::size_t total_bytes = 0;
    std::size_t total_packets = 0;

    for (std::size_t i = 0; i < files.size(); ++i) {
        cv::Mat original = cv::imread(files[i].string(), cv::IMREAD_COLOR);
        if (original.empty()) continue;

        encoder.pushImage(original, jpeg_bytes, key_period);

        cv::Mat decoded;
        bool produced_frame = false;
        bool was_keyframe = false;
        std::size_t frame_bytes = 0;
        std::size_t frame_packets = 0;

        std::vector<u_char> chunk;
        while (encoder.getNextChunk(chunk)) {
            ++frame_packets;
            ++total_packets;
            frame_bytes += chunk.size();
            total_bytes += chunk.size();

            if (chunk.size() > affinecodec::kMaxUdpPacketBytes) {
                std::cerr << "ERROR: packet " << chunk.size() << " B exceeds codec limit\n";
                return 3;
            }

            decoder.pushData(chunk);

            std::vector<u_char> new_jpeg;
            if (decoder.updateKeyframe(new_jpeg)) {
                current_jpeg = std::move(new_jpeg);
                const std::vector<PatchData> no_patch;
                decoder.render(decoded, no_patch, current_jpeg);
                produced_frame = !decoded.empty();
                was_keyframe = true;
            }

            std::vector<PatchData> patch;
            while (decoder.getNextPatch(patch)) {
                decoder.render(decoded, patch, current_jpeg);
                produced_frame = !decoded.empty();
                was_keyframe = false;
            }
        }

        if (!produced_frame) {
            std::cout << std::setw(4) << i << "  no decoded frame  "
                      << frame_packets << " packets  " << frame_bytes << " B\n";
            continue;
        }

        if (decoded.channels() == 1)
            cv::cvtColor(decoded, decoded, cv::COLOR_GRAY2BGR);
        if (decoded.size() != original.size())
            cv::resize(decoded, decoded, original.size());

        const double mae = colorMae(original, decoded);
        const std::string original_info =
            "frame " + std::to_string(i) + "  " + files[i].filename().string();
        const std::string decoded_info =
            std::string(was_keyframe ? "KEY  " : "PATCH  ") +
            std::to_string(frame_packets) + " pkt  " +
            std::to_string(frame_bytes) + " B  MAE=" + cv::format("%.2f", mae);

        cv::Mat left = addLabel(original, "ORIGINAL", original_info);
        cv::Mat right = addLabel(decoded, "DECODED", decoded_info);
        cv::Mat side;
        cv::hconcat(left, right, side);

        char name[64];
        std::snprintf(name, sizeof(name), "%04zu.jpg", i);
        cv::imwrite((output_dir / name).string(), side,
                    {cv::IMWRITE_JPEG_QUALITY, 95});

        std::cout << std::setw(4) << i
                  << (was_keyframe ? "  KEY   " : "  PATCH ")
                  << std::setw(3) << frame_packets << " pkt  "
                  << std::setw(7) << frame_bytes << " B"
                  << "  MAE=" << std::fixed << std::setprecision(2) << mae << '\n';
    }

    std::cout << "\nOutput: " << output_dir << '\n'
              << "Total codec packets: " << total_packets << '\n'
              << "Total codec bytes: " << total_bytes << '\n'
              << "Every getNextChunk() result is <= "
              << affinecodec::kMaxUdpPacketBytes << " bytes.\n";
    return 0;
}

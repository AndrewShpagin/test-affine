#include "udp_image_codec.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using affinecodec::Decoder;
using affinecodec::Encoder;
using affinecodec::EncoderTiming;
using affinecodec::KeyframeCodec;
using affinecodec::LKDebugData;
using affinecodec::PatchData;
using affinecodec::u_char;
using Clock = std::chrono::steady_clock;

constexpr bool kReusePreviousFrameBorders = true;
constexpr bool kMosaicKeyframes = true;
constexpr bool kUseJpeg2000 = false;
constexpr KeyframeCodec kDefaultKeyframeCodec =
    kUseJpeg2000 ? KeyframeCodec::Jpeg2000 : KeyframeCodec::Jpeg;

static double ms(const Clock::time_point& a, const Clock::time_point& b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

static double profiledEncoderMs(const EncoderTiming& t) {
    return t.prep_ms + t.color_norm_ms + t.jpeg_ms + t.chunk_ms +
           t.features_ms + t.ref_pyramid_ms + t.cur_pyramid_ms +
           t.predictor_ms + t.lk_forward_ms + t.lk_backward_ms +
           t.affine_ms + t.mesh_ms + t.serialize_ms;
}

static void addEncoderTiming(EncoderTiming& sum, const EncoderTiming& t) {
    sum.prep_ms += t.prep_ms;
    sum.color_norm_ms += t.color_norm_ms;
    sum.jpeg_ms += t.jpeg_ms;
    sum.chunk_ms += t.chunk_ms;
    sum.features_ms += t.features_ms;
    sum.feature_copy_ms += t.feature_copy_ms;
    sum.feature_downsample_ms += t.feature_downsample_ms;
    sum.feature_gftt_ms += t.feature_gftt_ms;
    sum.feature_bucket_ms += t.feature_bucket_ms;
    sum.ref_pyramid_ms += t.ref_pyramid_ms;
    sum.cur_pyramid_ms += t.cur_pyramid_ms;
    sum.predictor_ms += t.predictor_ms;
    sum.lk_forward_ms += t.lk_forward_ms;
    sum.lk_backward_ms += t.lk_backward_ms;
    sum.affine_ms += t.affine_ms;
    sum.mesh_ms += t.mesh_ms;
    sum.serialize_ms += t.serialize_ms;
}

static EncoderTiming divideEncoderTiming(const EncoderTiming& sum, double n, bool keyframe) {
    EncoderTiming t;
    t.keyframe = keyframe;
    if (n <= 0.0) return t;
    t.prep_ms = sum.prep_ms / n;
    t.color_norm_ms = sum.color_norm_ms / n;
    t.jpeg_ms = sum.jpeg_ms / n;
    t.chunk_ms = sum.chunk_ms / n;
    t.features_ms = sum.features_ms / n;
    t.feature_copy_ms = sum.feature_copy_ms / n;
    t.feature_downsample_ms = sum.feature_downsample_ms / n;
    t.feature_gftt_ms = sum.feature_gftt_ms / n;
    t.feature_bucket_ms = sum.feature_bucket_ms / n;
    t.ref_pyramid_ms = sum.ref_pyramid_ms / n;
    t.cur_pyramid_ms = sum.cur_pyramid_ms / n;
    t.predictor_ms = sum.predictor_ms / n;
    t.lk_forward_ms = sum.lk_forward_ms / n;
    t.lk_backward_ms = sum.lk_backward_ms / n;
    t.affine_ms = sum.affine_ms / n;
    t.mesh_ms = sum.mesh_ms / n;
    t.serialize_ms = sum.serialize_ms / n;
    return t;
}

static void printEncoderTiming(const EncoderTiming& t, double total_ms,
                               const std::string& prefix = "      ") {
    std::cout << prefix << "enc-prof:" << std::fixed << std::setprecision(2);
    if (t.keyframe) {
        std::cout << " prep=" << t.prep_ms
                  << " color=" << t.color_norm_ms
                  << " codec=" << t.jpeg_ms
                  << " chunk=" << t.chunk_ms
                  << " feat=" << t.features_ms
                  << "(copy=" << t.feature_copy_ms
                  << " down=" << t.feature_downsample_ms
                  << " GFTT=" << t.feature_gftt_ms
                  << " bucket=" << t.feature_bucket_ms << ")"
                  << " refPyr=" << t.ref_pyramid_ms;
        const double fallback_patch = t.cur_pyramid_ms + t.predictor_ms + t.lk_forward_ms +
                                      t.lk_backward_ms + t.affine_ms + t.mesh_ms + t.serialize_ms;
        if (fallback_patch > 0.01)
            std::cout << " fallbackPatch=" << fallback_patch;
    } else {
        std::cout << " prep=" << t.prep_ms
                  << " curPyr=" << t.cur_pyramid_ms
                  << " pred=" << t.predictor_ms
                  << " LKf=" << t.lk_forward_ms
                  << " LKb=" << t.lk_backward_ms
                  << " affine=" << t.affine_ms
                  << " mesh=" << t.mesh_ms
                  << " ser=" << t.serialize_ms;
        if (t.predictor_used) std::cout << " predictor=yes";
    }
    const double other = std::max(0.0, total_ms - profiledEncoderMs(t));
    std::cout << " other=" << other << " ms\n";
}

static const char* keyframeCodecName(KeyframeCodec codec) {
    return codec == KeyframeCodec::Jpeg2000 ? "JPEG2000" : "JPEG";
}

static bool parseKeyframeCodec(std::string value, KeyframeCodec& codec) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "jpeg" || value == "jpg") {
        codec = KeyframeCodec::Jpeg;
        return true;
    }
    if (value == "jpeg2000" || value == "jp2") {
        codec = KeyframeCodec::Jpeg2000;
        return true;
    }
    return false;
}

static std::string codecSizeInfo(const EncoderTiming& t) {
    if (!t.keyframe || t.jpeg_layer_bytes[0] == 0) return std::string();
    const bool jp2 = t.keyframe_codec == KeyframeCodec::Jpeg2000;
    std::string info = jp2 ? " JP2=" : " J=";
    if (t.mosaic_keyframe) {
        info += cv::format("%.1f", t.jpeg_layer_bytes[0] / 1024.0) + "/" +
                cv::format("%.1f", t.jpeg_layer_bytes[1] / 1024.0) + "/" +
                cv::format("%.1f", t.jpeg_layer_bytes[2] / 1024.0) + "k";
    } else {
        info += cv::format("%.1f", t.jpeg_layer_bytes[0] / 1024.0) + "k";
    }
    if (jp2 && t.jpeg2000_compression_x1000 > 0)
        info += " C=" + std::to_string(t.jpeg2000_compression_x1000);
    if (!jp2 && t.jpeg_quality > 0)
        info += " Q=" + std::to_string(t.jpeg_quality);
    if (t.jpeg_size.width > 0 && t.jpeg_size.height > 0)
        info += " " + std::to_string(t.jpeg_size.width) + "x" + std::to_string(t.jpeg_size.height);
    return info;
}

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

static cv::Mat makeLkPanel(const LKDebugData& debug) {
    cv::Mat out;
    if (debug.image.empty()) return out;
    if (debug.image.channels() == 1) cv::cvtColor(debug.image, out, cv::COLOR_GRAY2BGR);
    else out = debug.image.clone();

    const std::size_t count = std::min(debug.from.size(), debug.to.size());
    for (std::size_t i = 0; i < count; ++i) {
        const cv::Point2f a = debug.from[i];
        const cv::Point2f b = debug.to[i];
        cv::circle(out, a, 2, cv::Scalar(0, 255, 255), cv::FILLED, cv::LINE_AA);
        if (cv::norm(b - a) > 0.05f)
            cv::arrowedLine(out, a, b, cv::Scalar(0, 255, 0), 1, cv::LINE_AA, 0, 0.25);
        cv::circle(out, b, 2, cv::Scalar(0, 0, 255), cv::FILLED, cv::LINE_AA);
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc > 6) {
        std::cerr << "Usage: codec_test [image-folder] [output-folder] [keyframe-bytes] [key-period] [jpeg|jp2]\n";
        return 2;
    }

    const fs::path input_dir = argc > 1 ? fs::path(argv[1]) : fs::path("local-data/frames");
    const fs::path output_root = argc > 2 ? fs::path(argv[2]) : fs::path("output/codec");
    const int keyframe_bytes = argc > 3 ? std::stoi(argv[3]) : 40000;
    const int key_period = argc > 4 ? std::stoi(argv[4]) : 5;
    KeyframeCodec keyframe_codec = kDefaultKeyframeCodec;
    if (argc > 5 && !parseKeyframeCodec(argv[5], keyframe_codec)) {
        std::cerr << "Unknown keyframe codec: " << argv[5] << " (use jpeg or jp2)\n";
        return 2;
    }

    std::string output_mode = kMosaicKeyframes ? "mosaic" : "classic";
    if (keyframe_codec == KeyframeCodec::Jpeg2000) output_mode += "-jp2";
    const fs::path output_dir = output_root / output_mode;

    if (!fs::exists(input_dir)) {
        std::cerr << "Input folder does not exist: " << input_dir << '\n';
        return 1;
    }
    if (keyframe_codec == KeyframeCodec::Jpeg2000 && !cv::haveImageWriter(".jp2")) {
        std::cerr << "JPEG2000 writer is not available in this OpenCV build\n";
        return 4;
    }

    const std::vector<fs::path> files = listImages(input_dir);
    if (files.empty()) {
        std::cerr << "No images in " << input_dir << '\n';
        return 1;
    }
    fs::create_directories(output_dir);

    std::cout << "Input folder: " << input_dir << '\n'
              << "Output folder: " << output_dir << '\n'
              << "Target keyframe bytes: " << keyframe_bytes << '\n'
              << "Keyframe period: " << key_period << '\n'
              << "Keyframe codec: " << keyframeCodecName(keyframe_codec) << '\n'
              << "MOSAIC keyframes: " << (kMosaicKeyframes ? "yes" : "no") << '\n'
              << "Reuse previous-frame borders: " << (kReusePreviousFrameBorders ? "yes" : "no") << '\n'
              << "Max packet bytes: " << affinecodec::kMaxUdpPacketBytes << '\n'
              << "Number of images: " << files.size() << "\n\n";

    Encoder encoder;
    Decoder decoder;
    encoder.setMosaicKeyframes(kMosaicKeyframes);
    encoder.setKeyframeCodec(keyframe_codec);
    decoder.setReusePreviousFrameBorders(kReusePreviousFrameBorders);
    std::vector<u_char> current_jpeg;
    std::size_t total_bytes = 0, total_packets = 0, timed_frames = 0;
    std::size_t key_frames = 0, patch_frames = 0;
    std::size_t predictor_patch_frames = 0;
    double total_enc_ms = 0.0, total_dec_ms = 0.0;
    double key_enc_ms = 0.0, key_dec_ms = 0.0, key_codec_decode_ms = 0.0;
    double patch_enc_ms = 0.0, patch_dec_ms = 0.0;
    EncoderTiming key_timing_sum;
    EncoderTiming patch_timing_sum;

    for (std::size_t i = 0; i < files.size(); ++i) {
        cv::Mat original = cv::imread(files[i].string(), cv::IMREAD_COLOR);
        if (original.empty()) continue;

        const auto enc_start = Clock::now();
        encoder.pushImage(original, keyframe_bytes, key_period);
        const double enc_ms = ms(enc_start, Clock::now());
        const EncoderTiming enc_timing = encoder.lastTiming();

        LKDebugData lk_debug;
        const bool have_lk_debug = encoder.getLastLKDebug(lk_debug);

        cv::Mat decoded;
        bool produced_frame = false;
        bool was_keyframe = false;
        std::size_t frame_bytes = 0, frame_packets = 0;
        double dec_ms = 0.0;
        double frame_codec_decode_ms = 0.0;

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

            const auto dec_start = Clock::now();
            decoder.pushData(chunk);

            std::vector<u_char> new_jpeg;
            if (decoder.updateKeyframe(new_jpeg)) {
                current_jpeg = std::move(new_jpeg);
                decoder.render(decoded, {}, current_jpeg);
                frame_codec_decode_ms = decoder.lastKeyframeImageDecodeMs();
                produced_frame = !decoded.empty();
                was_keyframe = true;
            }

            std::vector<PatchData> patch;
            while (decoder.getNextPatch(patch)) {
                decoder.render(decoded, patch, current_jpeg);
                produced_frame = !decoded.empty();
                was_keyframe = false;
            }
            dec_ms += ms(dec_start, Clock::now());
        }

        ++timed_frames;
        total_enc_ms += enc_ms;
        total_dec_ms += dec_ms;

        if (!produced_frame) {
            std::cout << std::setw(4) << i << "  no decoded frame  "
                      << frame_packets << " packets  " << frame_bytes << " B"
                      << "  enc=" << std::fixed << std::setprecision(2) << enc_ms << " ms"
                      << "  dec=" << dec_ms << " ms\n";
            printEncoderTiming(enc_timing, enc_ms);
            continue;
        }

        if (was_keyframe) {
            ++key_frames; key_enc_ms += enc_ms; key_dec_ms += dec_ms;
            key_codec_decode_ms += frame_codec_decode_ms;
            addEncoderTiming(key_timing_sum, enc_timing);
        } else {
            ++patch_frames; patch_enc_ms += enc_ms; patch_dec_ms += dec_ms;
            addEncoderTiming(patch_timing_sum, enc_timing);
            if (enc_timing.predictor_used) ++predictor_patch_frames;
        }

        if (decoded.channels() == 1) cv::cvtColor(decoded, decoded, cv::COLOR_GRAY2BGR);
        if (decoded.size() != original.size()) cv::resize(decoded, decoded, original.size());

        const double mae = colorMae(original, decoded);
        const std::string original_info =
            "frame " + std::to_string(i) + "  " + files[i].filename().string();
        std::string decoded_info =
            std::string(was_keyframe ? "KEY  " : "PATCH  ") +
            std::to_string(frame_packets) + " pkt  " + std::to_string(frame_bytes) +
            " B" + codecSizeInfo(enc_timing) +
            " E=" + cv::format("%.1f", enc_ms) + "ms D=" + cv::format("%.1f", dec_ms) + "ms";
        if (was_keyframe)
            decoded_info += " CD=" + cv::format("%.1f", frame_codec_decode_ms) + "ms";

        cv::Mat left = addLabel(original, "ORIGINAL", original_info);
        cv::Mat right = addLabel(decoded, "DECODED", decoded_info);
        cv::Mat side;

        if (have_lk_debug && !lk_debug.image.empty()) {
            cv::Mat lk = makeLkPanel(lk_debug);
            if (lk.size() != original.size()) cv::resize(lk, lk, original.size(), 0.0, 0.0, cv::INTER_NEAREST);
            cv::Scalar lk_mean, lk_stddev;
            cv::meanStdDev(lk_debug.image, lk_mean, lk_stddev);
            const std::string lk_info =
                std::string(was_keyframe ? "features " : "FB-valid ") +
                std::to_string(std::min(lk_debug.from.size(), lk_debug.to.size())) +
                "  mean=" + cv::format("%.1f", lk_mean[0]) +
                " std=" + cv::format("%.1f", lk_stddev[0]);
            lk = addLabel(lk, "LK NORMALIZED", lk_info);
            std::vector<cv::Mat> panels{left, right, lk};
            cv::hconcat(panels, side);
        } else {
            cv::hconcat(left, right, side);
        }

        char name[64];
        std::snprintf(name, sizeof(name), "%04zu.png", i);
        cv::imwrite((output_dir / name).string(), side);

        std::cout << std::setw(4) << i
                  << (was_keyframe ? "  KEY   " : "  PATCH ")
                  << std::setw(3) << frame_packets << " pkt  "
                  << std::setw(7) << frame_bytes << " B"
                  << codecSizeInfo(enc_timing)
                  << "  MAE=" << std::fixed << std::setprecision(2) << mae
                  << "  enc=" << enc_ms << " ms"
                  << "  dec=" << dec_ms << " ms";
        if (was_keyframe) std::cout << "  codec-dec=" << frame_codec_decode_ms << " ms";
        std::cout << '\n';
        printEncoderTiming(enc_timing, enc_ms);
    }

    std::cout << "\nOutput: " << output_dir << '\n'
              << "Total codec packets: " << total_packets << '\n'
              << "Total codec bytes: " << total_bytes << '\n';

    if (timed_frames) {
        std::cout << "Average encode: " << std::fixed << std::setprecision(2)
                  << total_enc_ms / timed_frames << " ms/frame\n"
                  << "Average decode: " << total_dec_ms / timed_frames << " ms/frame\n";
    }
    if (key_frames) {
        const double avg_enc = key_enc_ms / key_frames;
        std::cout << "Keyframe average: encode=" << avg_enc
                  << " ms  decode=" << key_dec_ms / key_frames
                  << " ms  codec-decode=" << key_codec_decode_ms / key_frames
                  << " ms  (" << key_frames << " frames)\n";
        const EncoderTiming avg = divideEncoderTiming(key_timing_sum, static_cast<double>(key_frames), true);
        printEncoderTiming(avg, avg_enc, "  KEY avg:   ");
    }
    if (patch_frames) {
        const double avg_enc = patch_enc_ms / patch_frames;
        std::cout << "Patch average:    encode=" << avg_enc
                  << " ms  decode=" << patch_dec_ms / patch_frames
                  << " ms  (" << patch_frames << " frames)\n";
        EncoderTiming avg = divideEncoderTiming(patch_timing_sum, static_cast<double>(patch_frames), false);
        avg.predictor_used = predictor_patch_frames > 0;
        printEncoderTiming(avg, avg_enc, "  PATCH avg: ");
        std::cout << "  Predictor used on " << predictor_patch_frames << "/"
                  << patch_frames << " patch frames\n";
    }

    std::cout << "Every getNextChunk() result is <= "
              << affinecodec::kMaxUdpPacketBytes << " bytes.\n";
    return 0;
}

#pragma once

#include <opencv2/core.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace affinecodec {
using u_char = unsigned char;
constexpr std::size_t kMaxUdpPacketBytes = 1300;

enum class KeyframeCodec : std::uint8_t {
    Jpeg = 0,
    Jpeg2000 = 1,
};

struct PatchData {
    std::uint32_t frame_id = 0;
    std::uint32_t keyframe_id = 0;
    cv::Size original_size;
    std::uint8_t grid_x = 0;
    std::uint8_t grid_y = 0;
    std::array<float, 6> affine{};
    bool homography = false;
    std::array<float, 2> perspective{};
    std::vector<cv::Point2f> mesh;
};

struct LKDebugData {
    cv::Mat image;
    std::vector<cv::Point2f> from;
    std::vector<cv::Point2f> to;
};

struct EncoderTiming {
    bool keyframe = false;
    bool predictor_used = false;
    bool mosaic_keyframe = false;
    bool strips_keyframe = false;
    KeyframeCodec keyframe_codec = KeyframeCodec::Jpeg;
    std::array<std::size_t, 3> jpeg_layer_bytes{};
    cv::Size jpeg_size;
    int jpeg_quality = 0;
    int jpeg2000_compression_x1000 = 0;
    double prep_ms = 0.0;
    double color_norm_ms = 0.0;
    double jpeg_ms = 0.0;
    double chunk_ms = 0.0;
    double features_ms = 0.0;
    double feature_copy_ms = 0.0;
    double feature_downsample_ms = 0.0;
    double feature_gftt_ms = 0.0;
    double feature_bucket_ms = 0.0;
    double ref_pyramid_ms = 0.0;
    double cur_pyramid_ms = 0.0;
    double predictor_ms = 0.0;
    double lk_forward_ms = 0.0;
    double lk_backward_ms = 0.0;
    double affine_ms = 0.0;
    double homography_ms = 0.0;
    double mesh_ms = 0.0;
    double serialize_ms = 0.0;
};

class Encoder {
public:
    void pushImage(cv::Mat& image, int desired_jpeg_size, int keyframe_once_in_N);
    bool getNextChunk(std::vector<u_char>& data);
    bool getLastLKDebug(LKDebugData& debug) const;
    const EncoderTiming& lastTiming() const { return last_timing_; }
    void setMosaicKeyframes(bool enabled) {
        mosaic_keyframes_ = enabled;
        if (enabled) strips_keyframes_ = false;
    }
    bool mosaicKeyframes() const { return mosaic_keyframes_; }
    void setStripsKeyframes(bool enabled) {
        strips_keyframes_ = enabled;
        if (enabled) mosaic_keyframes_ = false;
    }
    bool stripsKeyframes() const { return strips_keyframes_; }
    void setKeyframeCodec(KeyframeCodec codec) { keyframe_codec_ = codec; }
    KeyframeCodec keyframeCodec() const { return keyframe_codec_; }
    void setHomographyTransform(bool enabled) { homography_transform_ = enabled; }
    bool homographyTransform() const { return homography_transform_; }
private:
    bool emitKeyframe(const cv::Mat& image, const cv::Mat& gray, int desired_jpeg_size, std::uint32_t frame_id);
    bool emitMosaicKeyframe(const cv::Mat& image, const cv::Mat& gray, int desired_jpeg_size, std::uint32_t frame_id);
    bool emitStripsKeyframe(const cv::Mat& image, const cv::Mat& gray, int desired_jpeg_size, std::uint32_t frame_id);
    bool estimatePatch(const cv::Mat& current_gray, std::uint32_t frame_id, PatchData& patch);
    void setReference(const cv::Mat& gray, std::uint32_t frame_id);
    cv::Size input_size_;
    cv::Mat reference_gray_;
    std::vector<cv::Point2f> reference_features_;
    std::vector<cv::Mat> reference_pyramid_;
    int reference_max_level_ = 0;
    std::vector<cv::Mat> current_pyramid_;
    std::vector<cv::Point2f> lk_forward_;
    std::vector<cv::Point2f> lk_back_;
    std::vector<unsigned char> lk_status_forward_;
    std::vector<unsigned char> lk_status_back_;
    std::vector<float> lk_error_forward_;
    std::vector<float> lk_error_back_;
    PatchData previous_patch_;
    bool have_previous_patch_ = false;
    double jpeg_bytes_per_pixel_ = 0.0;
    int jpeg_model_channels_ = 0;
    double mosaic_jpeg_bytes_per_pixel_ = 0.0;
    int mosaic_jpeg_model_channels_ = 0;
    double strips_jpeg_bytes_per_pixel_ = 0.0;
    int strips_jpeg_model_channels_ = 0;
    KeyframeCodec keyframe_codec_ = KeyframeCodec::Jpeg;
    bool mosaic_keyframes_ = false;
    bool strips_keyframes_ = false;
    bool homography_transform_ = false;
    std::uint32_t next_frame_id_ = 0;
    std::uint32_t keyframe_id_ = 0;
    int frames_since_keyframe_ = 0;
    bool have_reference_ = false;
    EncoderTiming last_timing_;
    std::deque<std::vector<u_char>> output_queue_;
};

class Decoder {
public:
    void pushData(const std::vector<u_char>& data);
    bool updateKeyframe(std::vector<u_char>& jpeg_data);
    bool getNextPatch(std::vector<PatchData>& patch);
    void render(cv::Mat& destination, const std::vector<PatchData>& patch, const std::vector<u_char>& jpeg_data);
    void setReusePreviousFrameBorders(bool enabled) { reuse_previous_frame_borders_ = enabled; }
    bool reusePreviousFrameBorders() const { return reuse_previous_frame_borders_; }
    cv::Size originalSize() const { return original_size_; }
    std::uint32_t keyframeId() const { return keyframe_id_; }
    double lastKeyframeImageDecodeMs() const { return last_keyframe_image_decode_ms_; }
private:
    struct KeyframeAssembly {
        bool active = false;
        std::uint32_t frame_id = 0;
        cv::Size original_size;
        cv::Size jpeg_size;
        std::uint32_t jpeg_bytes = 0;
        std::uint16_t chunk_count = 0;
        std::uint16_t received_count = 0;
        std::vector<u_char> bytes;
        std::vector<unsigned char> received;
    };
    struct MosaicLayerAssembly {
        cv::Size jpeg_size;
        std::uint32_t jpeg_bytes = 0;
        std::uint16_t chunk_count = 0;
        std::uint16_t received_count = 0;
        std::vector<u_char> bytes;
        std::vector<unsigned char> received;
        cv::Mat decoded;
        bool complete = false;
    };
    struct MosaicAssembly {
        bool active = false;
        std::uint32_t frame_id = 0;
        cv::Size original_size;
        std::uint8_t layer_count = 0;
        bool end_received = false;
        double image_decode_ms = 0.0;
        std::array<MosaicLayerAssembly, 3> layers;
    };
    cv::Mat getDecodedKeyframe(const std::vector<u_char>& jpeg_data);
    void resetPendingKeyframe();
    bool decodeMosaicLayer(std::uint8_t layer_index);
    bool rebuildMosaicKeyframe();
    cv::Size original_size_;
    std::uint32_t keyframe_id_ = 0;
    bool have_keyframe_ = false;
    std::vector<u_char> current_jpeg_;
    bool keyframe_changed_ = false;
    cv::Mat decoded_keyframe_;
    KeyframeAssembly pending_keyframe_;
    MosaicAssembly pending_mosaic_;
    std::deque<PatchData> pending_patch_queue_;
    std::deque<PatchData> patch_queue_;
    cv::Mat dense_mesh_;
    cv::Mat map_x_;
    cv::Mat map_y_;
    cv::Mat valid_mask_;
    cv::Mat previous_render_;
    bool reuse_previous_frame_borders_ = false;
    double last_keyframe_image_decode_ms_ = 0.0;
};
} // namespace affinecodec

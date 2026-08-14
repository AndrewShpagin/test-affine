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

struct PatchData {
    std::uint32_t frame_id = 0;
    std::uint32_t keyframe_id = 0;
    cv::Size original_size;
    std::uint8_t grid_x = 0;
    std::uint8_t grid_y = 0;
    std::array<float, 6> affine{};
    std::vector<cv::Point2f> mesh;
};

struct LKDebugData {
    cv::Mat image;
    std::vector<cv::Point2f> from;
    std::vector<cv::Point2f> to;
};

class Encoder {
public:
    void pushImage(cv::Mat& image, int desired_jpeg_size, int keyframe_once_in_N);
    bool getNextChunk(std::vector<u_char>& data);
    bool getLastLKDebug(LKDebugData& debug) const;
private:
    bool emitKeyframe(const cv::Mat& image, const cv::Mat& gray, int desired_jpeg_size, std::uint32_t frame_id);
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
    cv::Size jpeg_size_hint_;
    int jpeg_budget_hint_ = 0;
    std::uint32_t next_frame_id_ = 0;
    std::uint32_t keyframe_id_ = 0;
    int frames_since_keyframe_ = 0;
    bool have_reference_ = false;
    std::deque<std::vector<u_char>> output_queue_;
};

class Decoder {
public:
    void pushData(const std::vector<u_char>& data);
    bool updateKeyframe(std::vector<u_char>& jpeg_data);
    bool getNextPatch(std::vector<PatchData>& patch);
    void render(cv::Mat& destination, const std::vector<PatchData>& patch, const std::vector<u_char>& jpeg_data);
    cv::Size originalSize() const { return original_size_; }
    std::uint32_t keyframeId() const { return keyframe_id_; }
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
    cv::Mat getDecodedKeyframe(const std::vector<u_char>& jpeg_data);
    void resetPendingKeyframe();
    cv::Size original_size_;
    std::uint32_t keyframe_id_ = 0;
    bool have_keyframe_ = false;
    std::vector<u_char> current_jpeg_;
    bool keyframe_changed_ = false;
    cv::Mat decoded_keyframe_;
    KeyframeAssembly pending_keyframe_;
    std::deque<PatchData> pending_patch_queue_;
    std::deque<PatchData> patch_queue_;
    cv::Mat dense_mesh_;
    cv::Mat map_x_;
    cv::Mat map_y_;
    cv::Mat valid_mask_;
};
} // namespace affinecodec

#pragma once

#include <opencv2/core.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace flowx {

struct FrameMetadata {
    std::uint64_t sequence = 0;
    std::uint32_t stream_id = 0;
    std::uint32_t frame_id = 0;
    std::uint32_t keyframe_id = 0;
    std::uint64_t capture_timestamp_us = 0;
    std::uint64_t receive_timestamp_us = 0;
    bool keyframe = false;
};

struct PublishedFrame {
    cv::Mat image;
    FrameMetadata metadata;
};

// Thread-safe immutable latest-frame publication point. The receiver core is the
// producer; the HTTP layer added later can take shared snapshots without cloning
// the image on every request.
class FrameStore {
public:
    void publish(cv::Mat image, FrameMetadata metadata);
    std::shared_ptr<const PublishedFrame> latest() const;

    bool waitForNext(std::uint64_t after_sequence,
                     std::shared_ptr<const PublishedFrame>& frame,
                     std::chrono::milliseconds timeout) const;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::shared_ptr<const PublishedFrame> latest_;
    std::uint64_t next_sequence_ = 1;
};

} // namespace flowx

#pragma once

#include "flowx_image_source.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace flowx {

class LatestFrame {
public:
    void publish(CapturedFrame&& frame) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frame_ = std::move(frame);
            ++sequence_;
        }
        condition_.notify_one();
    }

    bool waitNext(std::uint64_t& last_sequence,
                  CapturedFrame& frame,
                  const std::atomic<bool>& stop) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] {
            return sequence_ != last_sequence || stop.load(std::memory_order_relaxed) || closed_;
        });
        if (sequence_ == last_sequence) return false;
        last_sequence = sequence_;
        frame.image = frame_.image;
        frame.capture_timestamp_us = frame_.capture_timestamp_us;
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    CapturedFrame frame_;
    std::uint64_t sequence_ = 0;
    bool closed_ = false;
};

} // namespace flowx

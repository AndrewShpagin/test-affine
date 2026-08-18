#pragma once

#include <cstdint>
#include <mutex>

namespace flowx {

struct ReceiverStatus {
    std::uint64_t started_timestamp_us = 0;
    std::uint32_t active_stream_id = 0;

    std::uint64_t received_datagrams = 0;
    std::uint64_t received_bytes = 0;
    std::uint64_t invalid_datagrams = 0;
    std::uint64_t ignored_datagrams = 0;
    std::uint64_t ignored_other_stream = 0;
    std::uint64_t stale_frames = 0;
    std::uint64_t stream_resets = 0;

    std::uint64_t decoded_frames = 0;
    std::uint64_t decoded_keyframes = 0;
    std::uint64_t decoded_patches = 0;
};

class ReceiverStatusStore {
public:
    void publish(const ReceiverStatus& status) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = status;
    }

    ReceiverStatus snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }

private:
    mutable std::mutex mutex_;
    ReceiverStatus status_;
};

} // namespace flowx

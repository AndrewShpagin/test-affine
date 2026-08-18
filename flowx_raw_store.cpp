#include "flowx_raw_store.h"

#include <utility>

namespace flowx {

bool RawFrameStore::frameIdNewer(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::int32_t>(a - b) > 0;
}

void RawFrameStore::startBundleLocked(const std::vector<u_char>& datagram,
                                      const PacketMetadata& metadata) {
    current_ = RawFrameBundle{};
    current_.stream_id = metadata.stream_id;
    current_.frame_id = metadata.frame_id;
    current_.keyframe_id = metadata.keyframe_id;
    current_.keyframe = metadata.frame_id == metadata.keyframe_id;
    current_.datagrams.push_back(datagram);
    have_current_ = true;
}

void RawFrameStore::publishCurrentLocked() {
    if (!have_current_ || current_.datagrams.empty()) return;
    current_.sequence = next_sequence_++;
    latest_ = std::make_shared<const RawFrameBundle>(std::move(current_));
    if (latest_->keyframe) latest_keyframe_ = latest_;
    current_ = RawFrameBundle{};
    have_current_ = false;
}

void RawFrameStore::push(const std::vector<u_char>& datagram,
                         const PacketMetadata& metadata) {
    if (datagram.empty()) return;

    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;

        if (!have_current_) {
            startBundleLocked(datagram, metadata);
            return;
        }

        if (metadata.stream_id != current_.stream_id) {
            current_ = RawFrameBundle{};
            have_current_ = false;
            latest_.reset();
            latest_keyframe_.reset();
            startBundleLocked(datagram, metadata);
            notify = true;
        } else if (metadata.frame_id == current_.frame_id) {
            current_.datagrams.push_back(datagram);
            return;
        } else if (!frameIdNewer(metadata.frame_id, current_.frame_id)) {
            return;
        } else {
            publishCurrentLocked();
            notify = true;
            startBundleLocked(datagram, metadata);
        }
    }
    if (notify) changed_.notify_all();
}

std::shared_ptr<const RawFrameBundle> RawFrameStore::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

std::shared_ptr<const RawFrameBundle> RawFrameStore::latestKeyframe() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_keyframe_;
}

bool RawFrameStore::waitForNext(std::uint64_t after_sequence,
                                std::shared_ptr<const RawFrameBundle>& bundle,
                                std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ready = changed_.wait_for(lock, timeout, [&] {
        return closed_ || (latest_ && latest_->sequence > after_sequence);
    });
    if (!ready || !latest_ || latest_->sequence <= after_sequence) return false;
    bundle = latest_;
    return true;
}

void RawFrameStore::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        publishCurrentLocked();
        closed_ = true;
    }
    changed_.notify_all();
}

} // namespace flowx

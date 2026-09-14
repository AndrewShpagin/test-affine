#include "flowx_raw_store.h"

#include <utility>

namespace flowx {
namespace {
unsigned u16(const std::vector<u_char>& b, unsigned p) {
    return b[p] | (static_cast<unsigned>(b[p + 1]) << 8);
}
std::uint64_t keySlot(const std::vector<u_char>& b) {
    const unsigned type = b[2] & 15;
    if (type == 4 && b.size() >= 36)
        return (4ull << 60) | (static_cast<std::uint64_t>(u16(b, 30)) << 16) | u16(b, 28);
    if (type == 1 && b.size() >= 34)
        return (1ull << 60) | ((b[3] & 3u) << 8) | b[32];
    return static_cast<std::uint64_t>(type) << 60;
}
}

bool RawFrameStore::frameIdNewer(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::int32_t>(a - b) > 0;
}

void RawFrameStore::push(const std::vector<u_char>& datagram,
                         const PacketMetadata& metadata) {
    if (datagram.size() < kFlowXHeaderBytes) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        if (metadata.stream_id != stream_id_) {
            stream_id_ = metadata.stream_id;
            key_ = RawFrameBundle{};
            have_key_ = false;
            key_bytes_ = 0;
            key_slots_.clear();
            latest_.reset();
            latest_patch_.reset();
        }
        const bool is_key = metadata.frame_id == metadata.keyframe_id;
        if (is_key) {
            if (have_key_ && metadata.frame_id != key_.frame_id &&
                !frameIdNewer(metadata.frame_id, key_.frame_id)) return;
            if (!have_key_ || metadata.frame_id != key_.frame_id) {
                key_ = RawFrameBundle{};
                key_.stream_id = metadata.stream_id;
                key_.frame_id = key_.keyframe_id = metadata.frame_id;
                key_.keyframe = true;
                have_key_ = true;
                key_bytes_ = 0;
                key_slots_.clear();
                if (latest_patch_ && latest_patch_->keyframe_id != key_.keyframe_id)
                    latest_patch_.reset();
            }
            const auto slot = keySlot(datagram);
            if (key_slots_.count(slot)) return;
            // Stay below the browser's 16 MiB record and 16-bit packet count.
            if (key_.datagrams.size() >= 65534 || key_bytes_ + datagram.size() > 12u * 1024u * 1024u)
                return;
            key_slots_.emplace(slot, key_.datagrams.size());
            key_.datagrams.push_back(datagram);
            key_bytes_ += datagram.size();
        } else {
            if (have_key_ && metadata.keyframe_id != key_.keyframe_id &&
                !frameIdNewer(metadata.keyframe_id, key_.keyframe_id)) return;
            if (latest_patch_ && !frameIdNewer(metadata.frame_id, latest_patch_->frame_id)) return;
        }
        RawFrameBundle update;
        update.sequence = next_sequence_++;
        update.stream_id = metadata.stream_id;
        update.frame_id = metadata.frame_id;
        update.keyframe_id = metadata.keyframe_id;
        update.keyframe = is_key;
        update.datagrams.push_back(datagram);
        latest_ = std::make_shared<const RawFrameBundle>(std::move(update));
        if (is_key) key_.sequence = latest_->sequence;
        else latest_patch_ = latest_;
    }
    changed_.notify_all();
}

std::shared_ptr<const RawFrameBundle> RawFrameStore::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

std::shared_ptr<const RawFrameBundle> RawFrameStore::latestKeyframe() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return have_key_ ? std::make_shared<const RawFrameBundle>(key_) : nullptr;
}

std::shared_ptr<const RawFrameBundle> RawFrameStore::snapshotLocked() const {
    if (!have_key_) return latest_;
    auto result = std::make_shared<RawFrameBundle>(key_);
    result->sequence = latest_->sequence;
    if (latest_patch_ && latest_patch_->keyframe_id == key_.keyframe_id) {
        result->datagrams.insert(result->datagrams.end(),
            latest_patch_->datagrams.begin(), latest_patch_->datagrams.end());
        result->frame_id = latest_patch_->frame_id;
        result->keyframe = false;
    }
    return result;
}

bool RawFrameStore::waitForNext(std::uint64_t after_sequence,
                                std::shared_ptr<const RawFrameBundle>& bundle,
                                std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ready = changed_.wait_for(lock, timeout, [&] {
        return closed_ || (latest_ && latest_->sequence > after_sequence);
    });
    if (!ready || !latest_ || latest_->sequence <= after_sequence) return false;
    bundle = after_sequence == 0 || latest_->sequence - after_sequence > 1
        ? snapshotLocked() : latest_;
    return true;
}

void RawFrameStore::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    changed_.notify_all();
}

} // namespace flowx

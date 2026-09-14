#pragma once

#include "flowx_protocol.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace flowx {

struct RawFrameBundle {
    std::uint64_t sequence = 0;
    std::uint32_t stream_id = 0;
    std::uint32_t frame_id = 0;
    std::uint32_t keyframe_id = 0;
    bool keyframe = false;
    std::vector<std::vector<u_char>> datagrams;
};

// Publishes each validated datagram immediately, including late key regions.
// Clients that skip updates receive the accumulated key plus the latest patch.
class RawFrameStore {
public:
    void push(const std::vector<u_char>& datagram, const PacketMetadata& metadata);
    std::shared_ptr<const RawFrameBundle> latest() const;
    std::shared_ptr<const RawFrameBundle> latestKeyframe() const;

    bool waitForNext(std::uint64_t after_sequence,
                     std::shared_ptr<const RawFrameBundle>& bundle,
                     std::chrono::milliseconds timeout) const;

    void close();

private:
    static bool frameIdNewer(std::uint32_t a, std::uint32_t b);
    std::shared_ptr<const RawFrameBundle> snapshotLocked() const;

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    RawFrameBundle key_;
    bool have_key_ = false;
    std::uint32_t stream_id_ = 0;
    std::size_t key_bytes_ = 0;
    std::unordered_map<std::uint64_t, std::size_t> key_slots_;
    std::shared_ptr<const RawFrameBundle> latest_;
    std::shared_ptr<const RawFrameBundle> latest_patch_;
    std::uint64_t next_sequence_ = 1;
    bool closed_ = false;
};

} // namespace flowx

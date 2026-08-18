#pragma once

#include "flowx_protocol.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
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

// Groups validated FlowX UDP datagrams by frame and publishes only the latest
// completed frame bundle. HTTP clients can therefore skip old frames without
// ever receiving half of a fragmented keyframe because they were slow.
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
    void startBundleLocked(const std::vector<u_char>& datagram,
                           const PacketMetadata& metadata);
    void publishCurrentLocked();

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    RawFrameBundle current_;
    bool have_current_ = false;
    std::shared_ptr<const RawFrameBundle> latest_;
    std::shared_ptr<const RawFrameBundle> latest_keyframe_;
    std::uint64_t next_sequence_ = 1;
    bool closed_ = false;
};

} // namespace flowx

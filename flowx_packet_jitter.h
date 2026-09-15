#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

namespace flowx {

struct PacketJitterStats {
    int min_ms = 0, max_ms = 0;
    std::uint32_t seed = 1;
    std::uint64_t scheduled = 0, delivered = 0, overflow = 0;
    std::size_t queued = 0;
};

// Independent per-datagram latency, ordered by deadline rather than FIFO.
// Explicit timestamps also allow deterministic tests without sleeping.
class PacketJitter {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr std::size_t kMaxPackets = 8192;
    static constexpr std::size_t kMaxBytes = 8*1024*1024;
    void reset(int min_ms, int max_ms, std::uint32_t seed);
    bool enabled() const { return stats_.max_ms > 0; }
    bool enqueue(std::vector<unsigned char> datagram, Time arrival);
    bool popReady(Time now, std::vector<unsigned char>& datagram);
    std::optional<Time> nextDue() const;
    PacketJitterStats stats() const;
private:
    struct Packet {
        Time due;
        std::uint64_t order;
        std::vector<unsigned char> data;
    };
    struct Later {
        bool operator()(const Packet& a, const Packet& b) const {
            return a.due == b.due ? a.order > b.order : a.due > b.due;
        }
    };
    std::vector<Packet> heap_;
    std::size_t bytes_ = 0;
    std::mt19937 random_{1};
    PacketJitterStats stats_;
};

} // namespace flowx

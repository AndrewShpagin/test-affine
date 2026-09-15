#include "flowx_packet_jitter.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace flowx {

void PacketJitter::reset(int min_ms, int max_ms, std::uint32_t seed) {
    if (min_ms < 0 || max_ms < min_ms || max_ms > 1000)
        throw std::invalid_argument("UDP jitter must satisfy 0 <= min <= max <= 1000 ms");
    heap_.clear(); bytes_ = 0; random_.seed(seed);
    stats_ = {}; stats_.min_ms = min_ms; stats_.max_ms = max_ms; stats_.seed = seed;
}

bool PacketJitter::enqueue(std::vector<unsigned char> data, Time arrival) {
    // Bound memory even if the stream is faster than the chosen delay allows.
    if (heap_.size() >= kMaxPackets || data.size() > kMaxBytes-bytes_) {
        ++stats_.overflow; return false;
    }
    const std::uint32_t span = static_cast<std::uint32_t>(stats_.max_ms-stats_.min_ms+1);
    // Rejection sampling avoids modulo bias and library-dependent distributions.
    const std::uint32_t threshold = (0u-span)%span;
    std::uint32_t sample;
    do { sample = random_(); } while (sample < threshold);
    const int delay_ms = stats_.min_ms+static_cast<int>(sample%span);
    bytes_ += data.size();
    heap_.push_back({arrival+std::chrono::milliseconds(delay_ms), stats_.scheduled++, std::move(data)});
    std::push_heap(heap_.begin(),heap_.end(),Later{});
    return true;
}

bool PacketJitter::popReady(Time now, std::vector<unsigned char>& data) {
    if (heap_.empty() || heap_.front().due > now) return false;
    std::pop_heap(heap_.begin(),heap_.end(),Later{});
    bytes_ -= heap_.back().data.size();
    data = std::move(heap_.back().data); heap_.pop_back(); ++stats_.delivered;
    return true;
}

std::optional<PacketJitter::Time> PacketJitter::nextDue() const {
    if (heap_.empty()) return std::nullopt;
    return heap_.front().due;
}

PacketJitterStats PacketJitter::stats() const {
    auto result = stats_; result.queued = heap_.size(); return result;
}

} // namespace flowx

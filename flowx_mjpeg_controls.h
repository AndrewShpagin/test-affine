#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

namespace flowx {

struct MjpegOptions {
    bool fill_gaps = true;
    bool smooth_fill = true;
    std::uint64_t revision = 0;
    std::uint64_t applied_revision = 0;
};

// HTTP workers only change the requested options. The receiver thread owns the
// decoder, applies a complete snapshot, and acknowledges it after redrawing.
class MjpegControls {
public:
    MjpegControls(bool fill_gaps, bool smooth_fill) {
        options_.fill_gaps = fill_gaps;
        options_.smooth_fill = smooth_fill;
    }

    MjpegOptions snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return options_;
    }

    MjpegOptions update(std::optional<bool> fill_gaps, std::optional<bool> smooth_fill) {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool fill = fill_gaps.value_or(options_.fill_gaps);
        const bool smooth = smooth_fill.value_or(options_.smooth_fill);
        if (fill != options_.fill_gaps || smooth != options_.smooth_fill) {
            options_.fill_gaps = fill;
            options_.smooth_fill = smooth;
            ++options_.revision;
        }
        return options_;
    }

    void applied(std::uint64_t revision) {
        std::lock_guard<std::mutex> lock(mutex_);
        options_.applied_revision = revision;
    }

private:
    mutable std::mutex mutex_;
    MjpegOptions options_;
};

} // namespace flowx

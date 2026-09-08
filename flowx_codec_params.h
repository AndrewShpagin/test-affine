#pragma once

#include "flowx_config.h"

#include <mutex>

namespace flowx {

// Thread-safe holder for the live codec parameters. The sender encode loop reads
// a consistent snapshot each frame while the HTTP control server mutates the
// parameters in memory; a mutex guarantees the reader never observes a torn
// CodecConfig.
class CodecParamsStore {
public:
    explicit CodecParamsStore(const CodecConfig& initial) : config_(initial) {}

    CodecConfig snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_;
    }

    void store(const CodecConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
    }

private:
    mutable std::mutex mutex_;
    CodecConfig config_;
};

} // namespace flowx

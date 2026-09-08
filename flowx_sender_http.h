#pragma once

#include "flowx_codec_params.h"
#include "flowx_config.h"

#include <memory>
#include <string>

namespace flowx {

// Lightweight HTTP control surface for the sender. It publishes the live codec
// parameters and accepts in-memory updates, so operators can retune keyframe
// sizing, cadence, and transform stages without editing the config file or
// restarting the sender. All mutations go through the CodecParamsStore mutex.
class SenderControlServer {
public:
    SenderControlServer();
    ~SenderControlServer();

    SenderControlServer(SenderControlServer&&) noexcept;
    SenderControlServer& operator=(SenderControlServer&&) noexcept;

    SenderControlServer(const SenderControlServer&) = delete;
    SenderControlServer& operator=(const SenderControlServer&) = delete;

    bool start(const SenderControlConfig& config, CodecParamsStore& params, std::string& error);
    void stop();
    bool isRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace flowx

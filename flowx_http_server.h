#pragma once

#include "flowx_config.h"
#include "flowx_frame_store.h"
#include "flowx_raw_store.h"
#include "flowx_receiver_status.h"
#include "flowx_mjpeg_controls.h"

#include <memory>
#include <string>

namespace flowx {

class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    HttpServer(HttpServer&&) noexcept;
    HttpServer& operator=(HttpServer&&) noexcept;

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool start(const HttpConfig& config,
               const FrameStore& frames,
               const RawFrameStore& raw_frames,
               const ReceiverStatusStore& status,
               MjpegControls& controls,
               std::string& error,
               const PlaybackConfig& playback = {});
    void stop();
    bool isRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace flowx

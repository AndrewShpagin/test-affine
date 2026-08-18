#pragma once

#include "flowx_config.h"
#include "flowx_frame_store.h"
#include "flowx_receiver_status.h"

#include <memory>
#include <string>

namespace flowx {

class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) = delete;
    HttpServer& operator=(HttpServer&&) = delete;

    bool start(const HttpConfig& config,
               const FrameStore& frames,
               const ReceiverStatusStore& status,
               std::string& error);
    void stop();
    bool isRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace flowx

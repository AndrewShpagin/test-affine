#pragma once

#include "flowx_config.h"
#include "flowx_protocol.h"

#include <memory>
#include <string>
#include <vector>

namespace flowx {

enum class UdpReceiveResult {
    Datagram,
    Timeout,
    Ignored,
    Error,
};

class UdpReceiver {
public:
    UdpReceiver();
    ~UdpReceiver();
    UdpReceiver(UdpReceiver&&) noexcept;
    UdpReceiver& operator=(UdpReceiver&&) noexcept;

    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    bool open(const UdpListenConfig& config, std::string& error);
    UdpReceiveResult receive(std::vector<u_char>& datagram,
                             int timeout_ms,
                             std::string& error);
    bool isOpen() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace flowx

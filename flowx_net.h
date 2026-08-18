#pragma once

#include "flowx_config.h"
#include "flowx_protocol.h"

#include <memory>
#include <string>
#include <vector>

namespace flowx {

class UdpSender {
public:
    UdpSender();
    ~UdpSender();
    UdpSender(UdpSender&&) noexcept;
    UdpSender& operator=(UdpSender&&) noexcept;

    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    bool open(const UdpTargetConfig& config, std::string& error);
    bool send(const std::vector<u_char>& datagram, std::string& error);
    bool isOpen() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace flowx

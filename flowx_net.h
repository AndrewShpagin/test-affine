#pragma once

#include "flowx_config.h"
#include "flowx_protocol.h"

#include <cstddef>
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
    // Configured MTU minus the connected address family's base IP + UDP headers.
    // A statistics threshold, not a send limit or a path MTU discovery result.
    std::size_t atomicPayloadBytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace flowx

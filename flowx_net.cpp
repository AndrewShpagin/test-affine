#include "flowx_net.h"

#include <cerrno>
#include <mutex>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace flowx {
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

bool ensureSocketSystem(std::string& error) {
    static std::once_flag once;
    static int startup_result = 0;
    std::call_once(once, [] {
        WSADATA data{};
        startup_result = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (startup_result != 0) {
        error = "WSAStartup failed: " + std::to_string(startup_result);
        return false;
    }
    return true;
}

void closeSocket(SocketHandle socket) {
    if (socket != kInvalidSocket) closesocket(socket);
}

int lastSocketError() { return WSAGetLastError(); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

bool ensureSocketSystem(std::string&) { return true; }

void closeSocket(SocketHandle socket) {
    if (socket != kInvalidSocket) ::close(socket);
}

int lastSocketError() { return errno; }
#endif

std::string socketErrorText(const char* operation) {
    return std::string(operation) + " failed (socket error " +
           std::to_string(lastSocketError()) + ")";
}

} // namespace

struct UdpSender::Impl {
    SocketHandle socket = kInvalidSocket;
    ~Impl() { closeSocket(socket); }
};

UdpSender::UdpSender() : impl_(std::make_unique<Impl>()) {}
UdpSender::~UdpSender() = default;
UdpSender::UdpSender(UdpSender&&) noexcept = default;
UdpSender& UdpSender::operator=(UdpSender&&) noexcept = default;

bool UdpSender::open(const UdpTargetConfig& config, std::string& error) {
    error.clear();
    if (!impl_) impl_ = std::make_unique<Impl>();

    closeSocket(impl_->socket);
    impl_->socket = kInvalidSocket;

    if (!ensureSocketSystem(error)) return false;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* result = nullptr;
    const std::string port = std::to_string(config.port);
    const int gai = getaddrinfo(config.host.c_str(), port.c_str(), &hints, &result);
    if (gai != 0) {
#ifdef _WIN32
        error = "getaddrinfo failed for " + config.host + ":" + port +
                " (" + std::to_string(gai) + ")";
#else
        error = "getaddrinfo failed for " + config.host + ":" + port +
                ": " + gai_strerror(gai);
#endif
        return false;
    }

    for (addrinfo* ai = result; ai; ai = ai->ai_next) {
        SocketHandle socket = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (socket == kInvalidSocket) continue;
#ifdef _WIN32
        const int address_length = static_cast<int>(ai->ai_addrlen);
#else
        const socklen_t address_length = static_cast<socklen_t>(ai->ai_addrlen);
#endif
        if (::connect(socket, ai->ai_addr, address_length) == 0) {
            impl_->socket = socket;
            break;
        }
        closeSocket(socket);
    }
    freeaddrinfo(result);

    if (impl_->socket == kInvalidSocket) {
        error = "cannot connect UDP socket to " + config.host + ":" + port;
        return false;
    }
    return true;
}

bool UdpSender::send(const std::vector<u_char>& datagram, std::string& error) {
    error.clear();
    if (!isOpen()) {
        error = "UDP sender is not open";
        return false;
    }
    if (datagram.empty() || datagram.size() > kMaxUdpDatagramBytes) {
        error = "invalid FlowX datagram size: " + std::to_string(datagram.size());
        return false;
    }

#ifdef _WIN32
    const int sent = ::send(impl_->socket,
                            reinterpret_cast<const char*>(datagram.data()),
                            static_cast<int>(datagram.size()), 0);
#else
    const int sent = static_cast<int>(
        ::send(impl_->socket, datagram.data(), datagram.size(), 0));
#endif
    if (sent != static_cast<int>(datagram.size())) {
        error = sent < 0 ? socketErrorText("UDP send") : "short UDP send";
        return false;
    }
    return true;
}

bool UdpSender::isOpen() const {
    return impl_ && impl_->socket != kInvalidSocket;
}

} // namespace flowx

#include "flowx_udp_receiver.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstring>
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
#include <sys/select.h>
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

void closeSocket(SocketHandle s) {
    if (s != kInvalidSocket) closesocket(s);
}

int lastSocketError() { return WSAGetLastError(); }
bool interruptedSocketError(int e) { return e == WSAEINTR; }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

bool ensureSocketSystem(std::string&) { return true; }

void closeSocket(SocketHandle s) {
    if (s != kInvalidSocket) ::close(s);
}

int lastSocketError() { return errno; }
bool interruptedSocketError(int e) { return e == EINTR; }
#endif

std::string socketErrorText(const char* operation) {
    return std::string(operation) + " failed (socket error " +
           std::to_string(lastSocketError()) + ")";
}

} // namespace

struct UdpReceiver::Impl {
    SocketHandle socket = kInvalidSocket;
    PacketJitter jitter;
    UdpReceiveResult receiveSocket(std::vector<u_char>& datagram, int timeout_ms, std::string& error);

    ~Impl() { closeSocket(socket); }
};

UdpReceiver::UdpReceiver() : impl_(std::make_unique<Impl>()) {}
UdpReceiver::~UdpReceiver() = default;
UdpReceiver::UdpReceiver(UdpReceiver&&) noexcept = default;
UdpReceiver& UdpReceiver::operator=(UdpReceiver&&) noexcept = default;

bool UdpReceiver::open(const UdpListenConfig& config, std::string& error) {
    error.clear();
    if (!impl_) impl_ = std::make_unique<Impl>();
    closeSocket(impl_->socket);
    impl_->socket = kInvalidSocket;
    if (config.jitter_min_ms < 0 || config.jitter_max_ms < config.jitter_min_ms || config.jitter_max_ms > 1000) {
        error = "UDP jitter must satisfy 0 <= min <= max <= 1000 ms"; return false;
    }
    impl_->jitter.reset(config.jitter_min_ms, config.jitter_max_ms, config.jitter_seed);

    if (!ensureSocketSystem(error)) return false;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_PASSIVE;

    const std::string port = std::to_string(config.port);
    addrinfo* result = nullptr;
    const char* bind_host = config.bind.empty() ? nullptr : config.bind.c_str();
    const int gai = getaddrinfo(bind_host, port.c_str(), &hints, &result);
    if (gai != 0) {
#ifdef _WIN32
        error = "getaddrinfo failed for UDP bind " + config.bind + ":" + port +
                " (" + std::to_string(gai) + ")";
#else
        error = "getaddrinfo failed for UDP bind " + config.bind + ":" + port +
                ": " + gai_strerror(gai);
#endif
        return false;
    }

    for (addrinfo* ai = result; ai; ai = ai->ai_next) {
        SocketHandle candidate = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (candidate == kInvalidSocket) continue;

        int reuse = 1;
#ifdef _WIN32
        setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        const int address_length = static_cast<int>(ai->ai_addrlen);
#else
        setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        const socklen_t address_length = static_cast<socklen_t>(ai->ai_addrlen);
#endif
        if (::bind(candidate, ai->ai_addr, address_length) == 0) {
            impl_->socket = candidate;
            break;
        }
        closeSocket(candidate);
    }
    freeaddrinfo(result);

    if (impl_->socket == kInvalidSocket) {
        error = "cannot bind UDP socket to " + config.bind + ":" + port;
        return false;
    }
    return true;
}

UdpReceiveResult UdpReceiver::receive(std::vector<u_char>& datagram,
                                      int timeout_ms,
                                      std::string& error) {
    datagram.clear();
    error.clear();
    if (!impl_ || impl_->socket == kInvalidSocket) {
        error = "UDP receiver is not open";
        return UdpReceiveResult::Error;
    }
    timeout_ms = std::max(0, timeout_ms);
    if (!impl_->jitter.enabled()) return impl_->receiveSocket(datagram, timeout_ms, error);

    using Clock = PacketJitter::Clock;
    const auto deadline = Clock::now()+std::chrono::milliseconds(timeout_ms);
    bool polled = false;
    for (;;) {
        const auto now = Clock::now();
        if (impl_->jitter.popReady(now, datagram)) return UdpReceiveResult::Datagram;
        if (polled && now >= deadline) return UdpReceiveResult::Timeout;
        auto wake = deadline;
        if (const auto due = impl_->jitter.nextDue()) wake = std::min(wake, *due);
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(wake-now).count();
        const int wait_ms = static_cast<int>(std::clamp<decltype(remaining)>(remaining, 0, timeout_ms));
        const auto result = impl_->receiveSocket(datagram, wait_ms, error);
        polled = true;
        if (result == UdpReceiveResult::Datagram) {
            if (!impl_->jitter.enqueue(std::move(datagram), Clock::now())) {
                datagram.clear(); error = "simulated UDP jitter queue full";
                return UdpReceiveResult::Ignored;
            }
            datagram.clear();
        } else if (result != UdpReceiveResult::Timeout) return result;
        // Continue accepting arrivals while older packets wait. Return by the
        // caller's deadline so key-assembly timers still run during UDP silence.
    }
}

UdpReceiveResult UdpReceiver::Impl::receiveSocket(std::vector<u_char>& datagram,
                                                int timeout_ms, std::string& error) {

    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket, &read_set);

    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

#ifdef _WIN32
    const int ready = ::select(0, &read_set, nullptr, nullptr, &timeout);
#else
    const int ready = ::select(socket + 1, &read_set, nullptr, nullptr, &timeout);
#endif
    if (ready == 0) return UdpReceiveResult::Timeout;
    if (ready < 0) {
        const int e = lastSocketError();
        if (interruptedSocketError(e)) return UdpReceiveResult::Timeout;
        error = socketErrorText("UDP select");
        return UdpReceiveResult::Error;
    }

    // Receive the complete UDP payload so an oversized/non-FlowX datagram can be
    // rejected explicitly rather than silently truncated to the FlowX limit.
    std::array<u_char, 65536> buffer{};
#ifdef _WIN32
    const int bytes = ::recvfrom(socket,
                                 reinterpret_cast<char*>(buffer.data()),
                                 static_cast<int>(buffer.size()),
                                 0, nullptr, nullptr);
#else
    const ssize_t bytes = ::recvfrom(socket, buffer.data(), buffer.size(),
                                     0, nullptr, nullptr);
#endif
    if (bytes < 0) {
        const int e = lastSocketError();
        if (interruptedSocketError(e)) return UdpReceiveResult::Timeout;
        error = socketErrorText("UDP recvfrom");
        return UdpReceiveResult::Error;
    }
    if (bytes == 0) {
        error = "empty UDP datagram";
        return UdpReceiveResult::Ignored;
    }

    const std::size_t size = static_cast<std::size_t>(bytes);
    if (size > kMaxUdpDatagramBytes) {
        error = "oversized UDP datagram: " + std::to_string(size) + " bytes";
        return UdpReceiveResult::Ignored;
    }

    datagram.assign(buffer.data(), buffer.data() + size);
    return UdpReceiveResult::Datagram;
}

bool UdpReceiver::isOpen() const {
    return impl_ && impl_->socket != kInvalidSocket;
}

PacketJitterStats UdpReceiver::jitterStats() const {
    return impl_ ? impl_->jitter.stats() : PacketJitterStats{};
}

} // namespace flowx

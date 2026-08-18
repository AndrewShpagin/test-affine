#pragma once

#include "flowx_codec.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace flowx {

// FlowX network envelope v3. The existing codec packet is kept intact as the
// payload, while the product transport adds stream/session identity and capture time.
constexpr std::uint8_t kProtocolVersion = 3;
constexpr std::size_t kFlowXHeaderBytes = 32;
constexpr std::size_t kMaxUdpDatagramBytes = 1400;
constexpr std::size_t kMaxFlowXPayloadBytes = kMaxUdpDatagramBytes - kFlowXHeaderBytes;

struct PacketMetadata {
    std::uint32_t stream_id = 0;
    std::uint32_t frame_id = 0;
    std::uint32_t keyframe_id = 0;
    std::uint64_t capture_timestamp_us = 0;
};

struct FlowXPacket {
    PacketMetadata metadata;
    std::vector<u_char> codec_packet;
};

std::uint32_t generateStreamId();

// Wrap one codec packet into a FlowX v3 UDP datagram. frame_id/keyframe_id are
// extracted from the codec packet so transport metadata cannot disagree with it.
bool wrapCodecPacket(const std::vector<u_char>& codec_packet,
                     std::uint32_t stream_id,
                     std::uint64_t capture_timestamp_us,
                     std::vector<u_char>& datagram,
                     std::string* error = nullptr);

// Parse a complete FlowX UDP datagram and return its transport metadata plus the
// original codec packet ready to pass to flowx::Decoder::pushData().
bool unwrapCodecPacket(const std::vector<u_char>& datagram,
                       FlowXPacket& packet,
                       std::string* error = nullptr);

} // namespace flowx

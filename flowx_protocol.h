#pragma once

#include "flowx_codec.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace flowx {

// FlowX v4 is the compact network wire format. The codec still uses AFC1 v2
// internally, but AFC1 headers are removed on the wire and reconstructed by the
// receiver adapter. All multibyte fields are little-endian.
constexpr std::uint8_t kProtocolVersion = 4;
constexpr std::uint16_t kFlowXMagic = 0x5846u; // bytes: 'F' 'X'
constexpr std::size_t kFlowXHeaderBytes = 20;  // common v4 header
constexpr std::size_t kMaxUdpDatagramBytes = 1400;
constexpr std::size_t kMaxFlowXPayloadBytes = kMaxUdpDatagramBytes - kFlowXHeaderBytes;
constexpr float kMeshWireScale = 128.0f;
constexpr float kMeshWireLimit = 255.0f;

enum class WirePacketType : std::uint8_t {
    KeyframeChunk = 1,
    Patch = 2,
    LayeredKeyframeEnd = 3,
};

struct PacketMetadata {
    std::uint32_t stream_id = 0;
    std::uint32_t frame_id = 0;
    std::uint32_t keyframe_id = 0;
    std::uint64_t capture_timestamp_us = 0;
};

struct FlowXPacket {
    PacketMetadata metadata;
    // Reconstructed AFC1 v2 packet for the existing C++ decoder. AFC1 is an
    // internal compatibility representation and is not present in the UDP data.
    std::vector<u_char> codec_packet;
};

std::uint32_t generateStreamId();

// Compact one internal AFC1 v2 packet into one FlowX v4 UDP datagram.
bool wrapCodecPacket(const std::vector<u_char>& codec_packet,
                     std::uint32_t stream_id,
                     std::uint64_t capture_timestamp_us,
                     std::vector<u_char>& datagram,
                     std::string* error = nullptr);

// Parse one FlowX v4 UDP datagram and reconstruct the AFC1 packet expected by
// flowx::Decoder::pushData().
bool unwrapCodecPacket(const std::vector<u_char>& datagram,
                       FlowXPacket& packet,
                       std::string* error = nullptr);

} // namespace flowx

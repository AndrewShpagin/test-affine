#include "flowx_protocol.h"

#include <chrono>
#include <cstring>
#include <random>

namespace flowx {
namespace {

// Little-endian bytes on the wire are: 'F' 'X' 'V' '3'.
constexpr std::uint32_t kFlowXMagic = 0x33565846u;
constexpr std::uint8_t kKnownFlags = 0;

// Current internal codec wire identifier. The FlowX envelope deliberately keeps
// this codec packet opaque after extracting frame identity.
constexpr std::uint32_t kCodecMagicV2 = 0x31434641u;
constexpr std::uint8_t kCodecVersionV2 = 2;
constexpr std::size_t kCodecCommonHeaderBytesV2 = 20;

void setError(std::string* error, const char* text) {
    if (error) *error = text;
}

void appendU8(std::vector<u_char>& out, std::uint8_t value) {
    out.push_back(value);
}

void appendU16(std::vector<u_char>& out, std::uint16_t value) {
    out.push_back(static_cast<u_char>(value & 0xffu));
    out.push_back(static_cast<u_char>((value >> 8) & 0xffu));
}

void appendU32(std::vector<u_char>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<u_char>((value >> shift) & 0xffu));
}

void appendU64(std::vector<u_char>& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<u_char>((value >> shift) & 0xffu));
}

bool readU8(const std::vector<u_char>& data, std::size_t& pos, std::uint8_t& value) {
    if (pos + 1 > data.size()) return false;
    value = data[pos++];
    return true;
}

bool readU16(const std::vector<u_char>& data, std::size_t& pos, std::uint16_t& value) {
    if (pos + 2 > data.size()) return false;
    value = static_cast<std::uint16_t>(data[pos]) |
            (static_cast<std::uint16_t>(data[pos + 1]) << 8);
    pos += 2;
    return true;
}

bool readU32(const std::vector<u_char>& data, std::size_t& pos, std::uint32_t& value) {
    if (pos + 4 > data.size()) return false;
    value = static_cast<std::uint32_t>(data[pos]) |
            (static_cast<std::uint32_t>(data[pos + 1]) << 8) |
            (static_cast<std::uint32_t>(data[pos + 2]) << 16) |
            (static_cast<std::uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return true;
}

bool readU64(const std::vector<u_char>& data, std::size_t& pos, std::uint64_t& value) {
    if (pos + 8 > data.size()) return false;
    value = 0;
    for (int shift = 0; shift < 64; shift += 8)
        value |= static_cast<std::uint64_t>(data[pos++]) << shift;
    return true;
}

bool inspectCodecPacket(const std::vector<u_char>& codec_packet,
                        std::uint32_t& frame_id,
                        std::uint32_t& keyframe_id) {
    if (codec_packet.size() < kCodecCommonHeaderBytesV2 ||
        codec_packet.size() > kMaxCodecPacketBytes)
        return false;

    std::size_t pos = 0;
    std::uint32_t magic = 0;
    std::uint8_t version = 0;
    std::uint8_t type = 0;
    std::uint16_t header_bytes = 0;
    if (!readU32(codec_packet, pos, magic) ||
        !readU8(codec_packet, pos, version) ||
        !readU8(codec_packet, pos, type) ||
        !readU16(codec_packet, pos, header_bytes) ||
        !readU32(codec_packet, pos, frame_id) ||
        !readU32(codec_packet, pos, keyframe_id))
        return false;

    (void)type;
    return magic == kCodecMagicV2 && version == kCodecVersionV2 &&
           header_bytes >= kCodecCommonHeaderBytesV2 && header_bytes <= codec_packet.size();
}

} // namespace

std::uint32_t generateStreamId() {
    std::random_device rd;
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::uint64_t mixed = static_cast<std::uint64_t>(now);
    mixed ^= static_cast<std::uint64_t>(rd()) << 32;
    mixed ^= static_cast<std::uint64_t>(rd());
    mixed ^= mixed >> 33;
    mixed *= 0xff51afd7ed558ccdULL;
    mixed ^= mixed >> 33;
    std::uint32_t id = static_cast<std::uint32_t>(mixed ^ (mixed >> 32));
    return id == 0 ? 1u : id;
}

bool wrapCodecPacket(const std::vector<u_char>& codec_packet,
                     std::uint32_t stream_id,
                     std::uint64_t capture_timestamp_us,
                     std::vector<u_char>& datagram,
                     std::string* error) {
    datagram.clear();
    if (stream_id == 0) {
        setError(error, "stream_id must be non-zero");
        return false;
    }
    if (codec_packet.size() > kMaxFlowXPayloadBytes) {
        setError(error, "codec packet is too large for FlowX UDP envelope");
        return false;
    }

    std::uint32_t frame_id = 0;
    std::uint32_t keyframe_id = 0;
    if (!inspectCodecPacket(codec_packet, frame_id, keyframe_id)) {
        setError(error, "invalid or unsupported codec packet");
        return false;
    }

    datagram.reserve(kFlowXHeaderBytes + codec_packet.size());
    appendU32(datagram, kFlowXMagic);
    appendU8(datagram, kProtocolVersion);
    appendU8(datagram, kKnownFlags);
    appendU16(datagram, static_cast<std::uint16_t>(kFlowXHeaderBytes));
    appendU32(datagram, stream_id);
    appendU32(datagram, frame_id);
    appendU32(datagram, keyframe_id);
    appendU64(datagram, capture_timestamp_us);
    appendU16(datagram, static_cast<std::uint16_t>(codec_packet.size()));
    appendU16(datagram, 0);
    datagram.insert(datagram.end(), codec_packet.begin(), codec_packet.end());

    if (datagram.size() > kMaxUdpDatagramBytes) {
        datagram.clear();
        setError(error, "FlowX datagram exceeds maximum size");
        return false;
    }
    if (error) error->clear();
    return true;
}

bool unwrapCodecPacket(const std::vector<u_char>& datagram,
                       FlowXPacket& packet,
                       std::string* error) {
    packet = FlowXPacket{};
    if (datagram.size() < kFlowXHeaderBytes || datagram.size() > kMaxUdpDatagramBytes) {
        setError(error, "invalid FlowX datagram size");
        return false;
    }

    std::size_t pos = 0;
    std::uint32_t magic = 0;
    std::uint8_t version = 0;
    std::uint8_t flags = 0;
    std::uint16_t header_bytes = 0;
    std::uint16_t payload_bytes = 0;
    std::uint16_t reserved = 0;

    if (!readU32(datagram, pos, magic) ||
        !readU8(datagram, pos, version) ||
        !readU8(datagram, pos, flags) ||
        !readU16(datagram, pos, header_bytes) ||
        !readU32(datagram, pos, packet.metadata.stream_id) ||
        !readU32(datagram, pos, packet.metadata.frame_id) ||
        !readU32(datagram, pos, packet.metadata.keyframe_id) ||
        !readU64(datagram, pos, packet.metadata.capture_timestamp_us) ||
        !readU16(datagram, pos, payload_bytes) ||
        !readU16(datagram, pos, reserved)) {
        setError(error, "truncated FlowX header");
        return false;
    }

    if (magic != kFlowXMagic || version != kProtocolVersion || flags != kKnownFlags ||
        header_bytes != kFlowXHeaderBytes || packet.metadata.stream_id == 0 || reserved != 0) {
        setError(error, "unsupported FlowX header");
        return false;
    }
    if (payload_bytes == 0 || payload_bytes > kMaxFlowXPayloadBytes ||
        static_cast<std::size_t>(header_bytes) + payload_bytes != datagram.size()) {
        setError(error, "invalid FlowX payload length");
        return false;
    }

    packet.codec_packet.assign(datagram.begin() + header_bytes, datagram.end());
    std::uint32_t codec_frame_id = 0;
    std::uint32_t codec_keyframe_id = 0;
    if (!inspectCodecPacket(packet.codec_packet, codec_frame_id, codec_keyframe_id) ||
        codec_frame_id != packet.metadata.frame_id ||
        codec_keyframe_id != packet.metadata.keyframe_id) {
        packet = FlowXPacket{};
        setError(error, "FlowX metadata does not match codec packet");
        return false;
    }

    if (error) error->clear();
    return true;
}

} // namespace flowx

#include "flowx_protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>

namespace flowx {
namespace {

// Internal AFC1 v2 representation. It is produced by the stable codec and
// reconstructed for the stable C++ decoder, but is no longer transmitted.
constexpr std::uint32_t kAfcMagic = 0x31434641u; // "AFC1"
constexpr std::uint8_t kAfcVersion = 2;
constexpr std::uint8_t kAfcKeyframeChunk = 1;
constexpr std::uint8_t kAfcPatch = 2;
constexpr std::uint8_t kAfcLayeredKeyframeChunk = 3;
constexpr std::uint8_t kAfcLayeredKeyframeEnd = 4;
constexpr std::uint16_t kAfcPatchFlagHomography = 1u << 0;
constexpr std::size_t kAfcCommonBytes = 20;
constexpr std::size_t kAfcKeyHeaderBytes = 40;
constexpr std::size_t kAfcLayerHeaderBytes = 44;
constexpr std::size_t kAfcLayerEndBytes = 24;
constexpr std::size_t kAfcClassicChunkCapacity = kMaxCodecPacketBytes - kAfcKeyHeaderBytes;
constexpr std::size_t kAfcLayerChunkCapacity = kMaxCodecPacketBytes - kAfcLayerHeaderBytes;

constexpr std::uint8_t kWireKeyLayerIndexMask = 0x03u;
constexpr std::uint8_t kWireKeyLayerCountMask = 0x0cu;
constexpr unsigned kWireKeyLayerCountShift = 2;
constexpr std::uint8_t kWirePatchHomography = 1u << 0;
constexpr std::uint8_t kWirePatchMesh = 1u << 1;
constexpr std::uint8_t kWirePatchKnownFlags = kWirePatchHomography | kWirePatchMesh;
constexpr int kMaxWireGrid = 8;

struct AfcCommon {
    std::uint8_t type = 0;
    std::uint16_t header_bytes = 0;
    std::uint32_t frame_id = 0;
    std::uint32_t keyframe_id = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

void setError(std::string* error, const char* text) {
    if (error) *error = text;
}

void appendU8(std::vector<u_char>& out, std::uint8_t v) { out.push_back(v); }
void appendU16(std::vector<u_char>& out, std::uint16_t v) {
    out.push_back(static_cast<u_char>(v & 0xffu));
    out.push_back(static_cast<u_char>((v >> 8) & 0xffu));
}
void appendI16(std::vector<u_char>& out, std::int16_t v) {
    appendU16(out, static_cast<std::uint16_t>(v));
}
void appendU32(std::vector<u_char>& out, std::uint32_t v) {
    out.push_back(static_cast<u_char>(v & 0xffu));
    out.push_back(static_cast<u_char>((v >> 8) & 0xffu));
    out.push_back(static_cast<u_char>((v >> 16) & 0xffu));
    out.push_back(static_cast<u_char>((v >> 24) & 0xffu));
}
void appendU64(std::vector<u_char>& out, std::uint64_t v) {
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<u_char>((v >> shift) & 0xffu));
}
void appendFloat(std::vector<u_char>& out, float v) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "32-bit float required");
    std::memcpy(&bits, &v, sizeof(bits));
    appendU32(out, bits);
}

bool readU8(const std::vector<u_char>& data, std::size_t& pos, std::uint8_t& v) {
    if (pos + 1 > data.size()) return false;
    v = data[pos++];
    return true;
}
bool readU16(const std::vector<u_char>& data, std::size_t& pos, std::uint16_t& v) {
    if (pos + 2 > data.size()) return false;
    v = static_cast<std::uint16_t>(data[pos]) |
        (static_cast<std::uint16_t>(data[pos + 1]) << 8);
    pos += 2;
    return true;
}
bool readI16(const std::vector<u_char>& data, std::size_t& pos, std::int16_t& v) {
    std::uint16_t u = 0;
    if (!readU16(data, pos, u)) return false;
    v = static_cast<std::int16_t>(u);
    return true;
}
bool readU32(const std::vector<u_char>& data, std::size_t& pos, std::uint32_t& v) {
    if (pos + 4 > data.size()) return false;
    v = static_cast<std::uint32_t>(data[pos]) |
        (static_cast<std::uint32_t>(data[pos + 1]) << 8) |
        (static_cast<std::uint32_t>(data[pos + 2]) << 16) |
        (static_cast<std::uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return true;
}
bool readU64(const std::vector<u_char>& data, std::size_t& pos, std::uint64_t& v) {
    if (pos + 8 > data.size()) return false;
    v = 0;
    for (int shift = 0; shift < 64; shift += 8)
        v |= static_cast<std::uint64_t>(data[pos++]) << shift;
    return true;
}
bool readFloat(const std::vector<u_char>& data, std::size_t& pos, float& v) {
    std::uint32_t bits = 0;
    if (!readU32(data, pos, bits)) return false;
    std::memcpy(&v, &bits, sizeof(v));
    return true;
}

bool readAfcCommon(const std::vector<u_char>& data, AfcCommon& h) {
    if (data.size() < kAfcCommonBytes || data.size() > kMaxCodecPacketBytes) return false;
    std::size_t pos = 0;
    std::uint32_t magic = 0;
    std::uint8_t version = 0;
    if (!readU32(data, pos, magic) || !readU8(data, pos, version) ||
        !readU8(data, pos, h.type) || !readU16(data, pos, h.header_bytes) ||
        !readU32(data, pos, h.frame_id) || !readU32(data, pos, h.keyframe_id) ||
        !readU16(data, pos, h.width) || !readU16(data, pos, h.height))
        return false;
    return magic == kAfcMagic && version == kAfcVersion &&
           h.header_bytes >= kAfcCommonBytes && h.header_bytes <= data.size() &&
           h.width > 0 && h.height > 0;
}

void appendAfcCommon(std::vector<u_char>& out,
                     std::uint8_t type,
                     std::uint16_t header_bytes,
                     std::uint32_t frame_id,
                     std::uint32_t keyframe_id,
                     std::uint16_t width,
                     std::uint16_t height) {
    appendU32(out, kAfcMagic);
    appendU8(out, kAfcVersion);
    appendU8(out, type);
    appendU16(out, header_bytes);
    appendU32(out, frame_id);
    appendU32(out, keyframe_id);
    appendU16(out, width);
    appendU16(out, height);
}

std::uint8_t makeVersionType(WirePacketType type) {
    return static_cast<std::uint8_t>((kProtocolVersion << 4) |
        (static_cast<std::uint8_t>(type) & 0x0fu));
}

void appendWireCommon(std::vector<u_char>& out,
                      WirePacketType type,
                      std::uint8_t flags,
                      std::uint32_t stream_id,
                      std::uint32_t frame_id,
                      std::uint64_t capture_timestamp_us) {
    appendU16(out, kFlowXMagic);
    appendU8(out, makeVersionType(type));
    appendU8(out, flags);
    appendU32(out, stream_id);
    appendU32(out, frame_id);
    appendU64(out, capture_timestamp_us);
}

bool readWireCommon(const std::vector<u_char>& data,
                    WirePacketType& type,
                    std::uint8_t& flags,
                    PacketMetadata& metadata) {
    if (data.size() < kFlowXHeaderBytes || data.size() > kMaxUdpDatagramBytes) return false;
    std::size_t pos = 0;
    std::uint16_t magic = 0;
    std::uint8_t version_type = 0;
    if (!readU16(data, pos, magic) || !readU8(data, pos, version_type) ||
        !readU8(data, pos, flags) || !readU32(data, pos, metadata.stream_id) ||
        !readU32(data, pos, metadata.frame_id) ||
        !readU64(data, pos, metadata.capture_timestamp_us))
        return false;
    if (magic != kFlowXMagic || (version_type >> 4) != kProtocolVersion ||
        metadata.stream_id == 0)
        return false;
    const std::uint8_t raw_type = version_type & 0x0fu;
    if (raw_type < static_cast<std::uint8_t>(WirePacketType::KeyframeChunk) ||
        raw_type > static_cast<std::uint8_t>(WirePacketType::LayeredKeyframeEnd))
        return false;
    type = static_cast<WirePacketType>(raw_type);
    return true;
}

std::uint8_t keyFlags(std::uint8_t layer_index, std::uint8_t layer_count) {
    return static_cast<std::uint8_t>((layer_index & kWireKeyLayerIndexMask) |
        (((layer_count - 1u) << kWireKeyLayerCountShift) & kWireKeyLayerCountMask));
}

bool decodeKeyFlags(std::uint8_t flags, std::uint8_t& layer_index, std::uint8_t& layer_count) {
    if ((flags & 0xf0u) != 0) return false;
    layer_index = flags & kWireKeyLayerIndexMask;
    layer_count = static_cast<std::uint8_t>(((flags & kWireKeyLayerCountMask) >>
        kWireKeyLayerCountShift) + 1u);
    return layer_count >= 1 && layer_count <= 3 && layer_index < layer_count;
}

std::int16_t quantizeMesh(float value) {
    const float clipped = std::clamp(value, -kMeshWireLimit, kMeshWireLimit);
    const long q = std::lround(clipped * kMeshWireScale);
    return static_cast<std::int16_t>(std::clamp<long>(
        q, std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
}

float dequantizeMesh(std::int16_t value) {
    return static_cast<float>(value) / kMeshWireScale;
}

bool wrapKeyChunk(const std::vector<u_char>& codec,
                  const AfcCommon& h,
                  std::uint32_t stream_id,
                  std::uint64_t capture_timestamp_us,
                  std::vector<u_char>& datagram,
                  std::string* error) {
    std::uint8_t layer_index = 0;
    std::uint8_t layer_count = 1;
    std::uint16_t jpeg_w = 0, jpeg_h = 0;
    std::uint16_t chunk_index16 = 0, chunk_count16 = 0, chunk_bytes = 0;
    std::uint32_t jpeg_bytes = 0, chunk_offset = 0;
    std::size_t pos = kAfcCommonBytes;

    if (h.keyframe_id != h.frame_id) {
        setError(error, "keyframe packet has different frame/keyframe ids");
        return false;
    }

    std::size_t expected_header = 0;
    std::size_t chunk_capacity = 0;
    if (h.type == kAfcKeyframeChunk) {
        expected_header = kAfcKeyHeaderBytes;
        chunk_capacity = kAfcClassicChunkCapacity;
        std::uint16_t reserved = 0;
        if (!readU16(codec, pos, jpeg_w) || !readU16(codec, pos, jpeg_h) ||
            !readU16(codec, pos, chunk_index16) || !readU16(codec, pos, chunk_count16) ||
            !readU32(codec, pos, jpeg_bytes) || !readU32(codec, pos, chunk_offset) ||
            !readU16(codec, pos, chunk_bytes) || !readU16(codec, pos, reserved) || reserved != 0) {
            setError(error, "invalid AFC1 classic keyframe chunk");
            return false;
        }
    } else {
        expected_header = kAfcLayerHeaderBytes;
        chunk_capacity = kAfcLayerChunkCapacity;
        std::uint8_t li = 0, lc = 0;
        std::uint16_t reserved0 = 0, reserved1 = 0;
        if (!readU8(codec, pos, li) || !readU8(codec, pos, lc) ||
            !readU16(codec, pos, reserved0) || !readU16(codec, pos, jpeg_w) ||
            !readU16(codec, pos, jpeg_h) || !readU16(codec, pos, chunk_index16) ||
            !readU16(codec, pos, chunk_count16) || !readU32(codec, pos, jpeg_bytes) ||
            !readU32(codec, pos, chunk_offset) || !readU16(codec, pos, chunk_bytes) ||
            !readU16(codec, pos, reserved1) || reserved0 != 0 || reserved1 != 0) {
            setError(error, "invalid AFC1 layered keyframe chunk");
            return false;
        }
        layer_index = li;
        layer_count = lc;
    }

    if (h.header_bytes != expected_header || pos != expected_header ||
        jpeg_w == 0 || jpeg_h == 0 || jpeg_bytes == 0 ||
        layer_count < 1 || layer_count > 3 || layer_index >= layer_count ||
        chunk_count16 == 0 || chunk_count16 > 255 || chunk_index16 >= chunk_count16 ||
        chunk_bytes == 0 || expected_header + chunk_bytes != codec.size() ||
        chunk_offset != static_cast<std::uint32_t>(chunk_index16 * chunk_capacity) ||
        static_cast<std::uint64_t>(chunk_offset) + chunk_bytes > jpeg_bytes) {
        setError(error, "AFC1 keyframe chunk cannot be represented by FlowX v4");
        return false;
    }

    datagram.clear();
    datagram.reserve(kFlowXHeaderBytes + 14 + chunk_bytes);
    appendWireCommon(datagram, WirePacketType::KeyframeChunk,
                     keyFlags(layer_index, layer_count), stream_id, h.frame_id,
                     capture_timestamp_us);
    appendU16(datagram, h.width);
    appendU16(datagram, h.height);
    appendU16(datagram, jpeg_w);
    appendU16(datagram, jpeg_h);
    appendU32(datagram, jpeg_bytes);
    appendU8(datagram, static_cast<std::uint8_t>(chunk_index16));
    appendU8(datagram, static_cast<std::uint8_t>(chunk_count16));
    datagram.insert(datagram.end(), codec.begin() + expected_header, codec.end());
    return true;
}

bool wrapPatch(const std::vector<u_char>& codec,
               const AfcCommon& h,
               std::uint32_t stream_id,
               std::uint64_t capture_timestamp_us,
               std::vector<u_char>& datagram,
               std::string* error) {
    if (h.type != kAfcPatch || h.header_bytes != codec.size() || codec.size() < 48) {
        setError(error, "invalid AFC1 patch");
        return false;
    }

    std::size_t pos = kAfcCommonBytes;
    std::uint8_t gx = 0, gy = 0;
    std::uint16_t afc_flags = 0;
    if (!readU8(codec, pos, gx) || !readU8(codec, pos, gy) ||
        !readU16(codec, pos, afc_flags) || gx == 0 || gy == 0 ||
        (afc_flags & ~kAfcPatchFlagHomography) != 0) {
        setError(error, "invalid AFC1 patch grid/flags");
        return false;
    }

    const bool homography = (afc_flags & kAfcPatchFlagHomography) != 0;
    const bool mesh_present = !(gx == 1 && gy == 1);
    if (mesh_present && (gx < 2 || gy < 2 || gx > kMaxWireGrid || gy > kMaxWireGrid)) {
        setError(error, "patch mesh grid is outside FlowX v4 range");
        return false;
    }

    std::array<float, 6> affine{};
    for (float& v : affine) {
        if (!readFloat(codec, pos, v)) {
            setError(error, "truncated AFC1 affine");
            return false;
        }
    }
    std::array<float, 2> perspective{};
    if (homography) {
        for (float& v : perspective) {
            if (!readFloat(codec, pos, v)) {
                setError(error, "truncated AFC1 homography");
                return false;
            }
        }
    }

    const std::size_t point_count = static_cast<std::size_t>(gx) * gy;
    if (pos + point_count * 2 * sizeof(float) != codec.size()) {
        setError(error, "invalid AFC1 mesh size");
        return false;
    }

    std::vector<std::int16_t> mesh_q;
    if (mesh_present) {
        mesh_q.reserve(point_count * 2);
        for (std::size_t i = 0; i < point_count * 2; ++i) {
            float v = 0.0f;
            if (!readFloat(codec, pos, v)) return false;
            mesh_q.push_back(quantizeMesh(v));
        }
    } else {
        // The product uses 1x1 zero mesh as its internal "mesh disabled" form.
        float x = 0.0f, y = 0.0f;
        if (!readFloat(codec, pos, x) || !readFloat(codec, pos, y) ||
            std::abs(x) > 1e-6f || std::abs(y) > 1e-6f) {
            setError(error, "1x1 AFC1 mesh must be zero");
            return false;
        }
    }

    const std::uint32_t key_age32 = h.frame_id - h.keyframe_id;
    if (key_age32 > std::numeric_limits<std::uint16_t>::max()) {
        setError(error, "keyframe is too old for compact FlowX keyframe_age");
        return false;
    }

    std::uint8_t flags = 0;
    if (homography) flags |= kWirePatchHomography;
    if (mesh_present) flags |= kWirePatchMesh;

    datagram.clear();
    datagram.reserve(kFlowXHeaderBytes + 39 + mesh_q.size() * sizeof(std::int16_t));
    appendWireCommon(datagram, WirePacketType::Patch, flags, stream_id, h.frame_id,
                     capture_timestamp_us);
    appendU16(datagram, static_cast<std::uint16_t>(key_age32));
    appendU16(datagram, h.width);
    appendU16(datagram, h.height);
    const std::uint8_t grid = mesh_present
        ? static_cast<std::uint8_t>(((gy - 1u) << 4) | (gx - 1u))
        : 0u;
    appendU8(datagram, grid);
    for (float v : affine) appendFloat(datagram, v);
    if (homography) for (float v : perspective) appendFloat(datagram, v);
    for (std::int16_t v : mesh_q) appendI16(datagram, v);
    return true;
}

bool wrapLayerEnd(const std::vector<u_char>& codec,
                  const AfcCommon& h,
                  std::uint32_t stream_id,
                  std::uint64_t capture_timestamp_us,
                  std::vector<u_char>& datagram,
                  std::string* error) {
    if (h.header_bytes != kAfcLayerEndBytes || codec.size() != kAfcLayerEndBytes ||
        h.frame_id != h.keyframe_id) {
        setError(error, "invalid AFC1 layered keyframe end");
        return false;
    }
    std::size_t pos = kAfcCommonBytes;
    std::uint8_t layer_count = 0, reserved0 = 0;
    std::uint16_t reserved1 = 0;
    if (!readU8(codec, pos, layer_count) || !readU8(codec, pos, reserved0) ||
        !readU16(codec, pos, reserved1) || layer_count < 2 || layer_count > 3 ||
        reserved0 != 0 || reserved1 != 0) {
        setError(error, "invalid AFC1 layered end fields");
        return false;
    }
    datagram.clear();
    appendWireCommon(datagram, WirePacketType::LayeredKeyframeEnd,
                     keyFlags(0, layer_count), stream_id, h.frame_id,
                     capture_timestamp_us);
    return true;
}

bool unwrapKeyChunk(const std::vector<u_char>& datagram,
                    std::uint8_t flags,
                    FlowXPacket& packet,
                    std::string* error) {
    std::uint8_t layer_index = 0, layer_count = 0;
    if (!decodeKeyFlags(flags, layer_index, layer_count)) {
        setError(error, "invalid FlowX keyframe layer flags");
        return false;
    }
    std::size_t pos = kFlowXHeaderBytes;
    std::uint16_t ow = 0, oh = 0, jw = 0, jh = 0;
    std::uint32_t jpeg_bytes = 0;
    std::uint8_t chunk_index = 0, chunk_count = 0;
    if (!readU16(datagram, pos, ow) || !readU16(datagram, pos, oh) ||
        !readU16(datagram, pos, jw) || !readU16(datagram, pos, jh) ||
        !readU32(datagram, pos, jpeg_bytes) || !readU8(datagram, pos, chunk_index) ||
        !readU8(datagram, pos, chunk_count) || ow == 0 || oh == 0 || jw == 0 || jh == 0 ||
        jpeg_bytes == 0 || chunk_count == 0 || chunk_index >= chunk_count || pos >= datagram.size()) {
        setError(error, "truncated FlowX keyframe chunk");
        return false;
    }

    const std::size_t chunk_bytes = datagram.size() - pos;
    const bool classic = layer_count == 1;
    const std::size_t capacity = classic ? kAfcClassicChunkCapacity : kAfcLayerChunkCapacity;
    const std::uint32_t chunk_offset = static_cast<std::uint32_t>(chunk_index * capacity);
    if (chunk_bytes > capacity || static_cast<std::uint64_t>(chunk_offset) + chunk_bytes > jpeg_bytes ||
        chunk_bytes > std::numeric_limits<std::uint16_t>::max()) {
        setError(error, "invalid FlowX keyframe chunk length");
        return false;
    }

    packet.metadata.keyframe_id = packet.metadata.frame_id;
    packet.codec_packet.clear();
    if (classic) {
        packet.codec_packet.reserve(kAfcKeyHeaderBytes + chunk_bytes);
        appendAfcCommon(packet.codec_packet, kAfcKeyframeChunk,
                        static_cast<std::uint16_t>(kAfcKeyHeaderBytes),
                        packet.metadata.frame_id, packet.metadata.frame_id, ow, oh);
        appendU16(packet.codec_packet, jw);
        appendU16(packet.codec_packet, jh);
        appendU16(packet.codec_packet, chunk_index);
        appendU16(packet.codec_packet, chunk_count);
        appendU32(packet.codec_packet, jpeg_bytes);
        appendU32(packet.codec_packet, chunk_offset);
        appendU16(packet.codec_packet, static_cast<std::uint16_t>(chunk_bytes));
        appendU16(packet.codec_packet, 0);
    } else {
        packet.codec_packet.reserve(kAfcLayerHeaderBytes + chunk_bytes);
        appendAfcCommon(packet.codec_packet, kAfcLayeredKeyframeChunk,
                        static_cast<std::uint16_t>(kAfcLayerHeaderBytes),
                        packet.metadata.frame_id, packet.metadata.frame_id, ow, oh);
        appendU8(packet.codec_packet, layer_index);
        appendU8(packet.codec_packet, layer_count);
        appendU16(packet.codec_packet, 0);
        appendU16(packet.codec_packet, jw);
        appendU16(packet.codec_packet, jh);
        appendU16(packet.codec_packet, chunk_index);
        appendU16(packet.codec_packet, chunk_count);
        appendU32(packet.codec_packet, jpeg_bytes);
        appendU32(packet.codec_packet, chunk_offset);
        appendU16(packet.codec_packet, static_cast<std::uint16_t>(chunk_bytes));
        appendU16(packet.codec_packet, 0);
    }
    packet.codec_packet.insert(packet.codec_packet.end(), datagram.begin() + pos, datagram.end());
    return packet.codec_packet.size() <= kMaxCodecPacketBytes;
}

bool unwrapPatch(const std::vector<u_char>& datagram,
                 std::uint8_t flags,
                 FlowXPacket& packet,
                 std::string* error) {
    if ((flags & ~kWirePatchKnownFlags) != 0) {
        setError(error, "unknown FlowX patch flags");
        return false;
    }
    const bool homography = (flags & kWirePatchHomography) != 0;
    const bool mesh_present = (flags & kWirePatchMesh) != 0;

    std::size_t pos = kFlowXHeaderBytes;
    std::uint16_t key_age = 0, ow = 0, oh = 0;
    std::uint8_t grid = 0;
    if (!readU16(datagram, pos, key_age) || !readU16(datagram, pos, ow) ||
        !readU16(datagram, pos, oh) || !readU8(datagram, pos, grid) || ow == 0 || oh == 0) {
        setError(error, "truncated FlowX patch header");
        return false;
    }
    packet.metadata.keyframe_id = packet.metadata.frame_id - key_age;

    std::array<float, 6> affine{};
    for (float& v : affine) {
        if (!readFloat(datagram, pos, v)) {
            setError(error, "truncated FlowX affine");
            return false;
        }
    }
    std::array<float, 2> perspective{};
    if (homography) {
        for (float& v : perspective) {
            if (!readFloat(datagram, pos, v)) {
                setError(error, "truncated FlowX homography");
                return false;
            }
        }
    }

    std::uint8_t gx = 1, gy = 1;
    std::vector<std::int16_t> mesh_q;
    if (mesh_present) {
        gx = static_cast<std::uint8_t>((grid & 0x0fu) + 1u);
        gy = static_cast<std::uint8_t>(((grid >> 4) & 0x0fu) + 1u);
        if (gx < 2 || gy < 2 || gx > kMaxWireGrid || gy > kMaxWireGrid) {
            setError(error, "invalid FlowX mesh grid");
            return false;
        }
        const std::size_t values = static_cast<std::size_t>(gx) * gy * 2;
        if (pos + values * sizeof(std::int16_t) != datagram.size()) {
            setError(error, "invalid FlowX mesh byte count");
            return false;
        }
        mesh_q.resize(values);
        for (std::int16_t& v : mesh_q) {
            if (!readI16(datagram, pos, v)) return false;
        }
    } else if (grid != 0 || pos != datagram.size()) {
        setError(error, "mesh-disabled FlowX patch has extra data");
        return false;
    }

    const std::size_t afc_points = mesh_present ? static_cast<std::size_t>(gx) * gy : 1u;
    const std::size_t afc_size = kAfcCommonBytes + 4 + 6 * sizeof(float) +
        (homography ? 2 * sizeof(float) : 0) + afc_points * 2 * sizeof(float);
    if (afc_size > kMaxCodecPacketBytes || afc_size > std::numeric_limits<std::uint16_t>::max()) {
        setError(error, "reconstructed AFC1 patch is too large");
        return false;
    }

    packet.codec_packet.clear();
    packet.codec_packet.reserve(afc_size);
    appendAfcCommon(packet.codec_packet, kAfcPatch, static_cast<std::uint16_t>(afc_size),
                    packet.metadata.frame_id, packet.metadata.keyframe_id, ow, oh);
    appendU8(packet.codec_packet, gx);
    appendU8(packet.codec_packet, gy);
    appendU16(packet.codec_packet, homography ? kAfcPatchFlagHomography : 0u);
    for (float v : affine) appendFloat(packet.codec_packet, v);
    if (homography) for (float v : perspective) appendFloat(packet.codec_packet, v);
    if (mesh_present) {
        for (std::int16_t v : mesh_q) appendFloat(packet.codec_packet, dequantizeMesh(v));
    } else {
        appendFloat(packet.codec_packet, 0.0f);
        appendFloat(packet.codec_packet, 0.0f);
    }
    return packet.codec_packet.size() == afc_size;
}

bool unwrapLayerEnd(std::uint8_t flags,
                    FlowXPacket& packet,
                    std::string* error) {
    std::uint8_t layer_index = 0, layer_count = 0;
    if (!decodeKeyFlags(flags, layer_index, layer_count) || layer_index != 0 || layer_count < 2) {
        setError(error, "invalid FlowX layered-end flags");
        return false;
    }
    packet.metadata.keyframe_id = packet.metadata.frame_id;
    packet.codec_packet.clear();
    packet.codec_packet.reserve(kAfcLayerEndBytes);
    // Original size is not used by the AFC1 end handler beyond common validation;
    // use 1x1 because v4 intentionally does not repeat keyframe dimensions here.
    appendAfcCommon(packet.codec_packet, kAfcLayeredKeyframeEnd,
                    static_cast<std::uint16_t>(kAfcLayerEndBytes),
                    packet.metadata.frame_id, packet.metadata.frame_id, 1, 1);
    appendU8(packet.codec_packet, layer_count);
    appendU8(packet.codec_packet, 0);
    appendU16(packet.codec_packet, 0);
    return true;
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
    if (error) error->clear();
    if (stream_id == 0) {
        setError(error, "stream_id must be non-zero");
        return false;
    }
    AfcCommon h;
    if (!readAfcCommon(codec_packet, h)) {
        setError(error, "invalid internal AFC1 packet");
        return false;
    }

    bool ok = false;
    switch (h.type) {
    case kAfcKeyframeChunk:
    case kAfcLayeredKeyframeChunk:
        ok = wrapKeyChunk(codec_packet, h, stream_id, capture_timestamp_us, datagram, error);
        break;
    case kAfcPatch:
        ok = wrapPatch(codec_packet, h, stream_id, capture_timestamp_us, datagram, error);
        break;
    case kAfcLayeredKeyframeEnd:
        ok = wrapLayerEnd(codec_packet, h, stream_id, capture_timestamp_us, datagram, error);
        break;
    default:
        setError(error, "unsupported internal AFC1 packet type");
        return false;
    }
    if (!ok) {
        datagram.clear();
        return false;
    }
    if (datagram.empty() || datagram.size() > kMaxUdpDatagramBytes) {
        datagram.clear();
        setError(error, "FlowX v4 datagram exceeds maximum size");
        return false;
    }
    return true;
}

bool unwrapCodecPacket(const std::vector<u_char>& datagram,
                       FlowXPacket& packet,
                       std::string* error) {
    packet = FlowXPacket{};
    if (error) error->clear();

    WirePacketType type{};
    std::uint8_t flags = 0;
    if (!readWireCommon(datagram, type, flags, packet.metadata)) {
        setError(error, "invalid FlowX v4 common header");
        return false;
    }

    bool ok = false;
    switch (type) {
    case WirePacketType::KeyframeChunk:
        ok = unwrapKeyChunk(datagram, flags, packet, error);
        break;
    case WirePacketType::Patch:
        ok = unwrapPatch(datagram, flags, packet, error);
        break;
    case WirePacketType::LayeredKeyframeEnd:
        if (datagram.size() != kFlowXHeaderBytes) {
            setError(error, "FlowX layered end must contain only common header");
            return false;
        }
        ok = unwrapLayerEnd(flags, packet, error);
        break;
    }
    if (!ok) packet = FlowXPacket{};
    return ok;
}

} // namespace flowx

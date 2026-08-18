#include "flowx_patch_rewrite.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace flowx {
namespace {

constexpr std::uint32_t kCodecMagic = 0x31434641u; // "AFC1"
constexpr std::uint8_t kCodecVersion = 2;
constexpr std::uint8_t kPatchType = 2;
constexpr std::uint16_t kPatchFlagHomography = 1u << 0;
constexpr std::uint16_t kPatchKnownFlags = kPatchFlagHomography;
constexpr std::size_t kCommonHeaderBytes = 20;
constexpr std::size_t kPatchFixedBytes = 4 + 6 * sizeof(float);
constexpr int kMaxProductMeshGrid = 8;

void setError(std::string* error, const std::string& text) {
    if (error) *error = text;
}

std::uint16_t readU16(const std::vector<u_char>& data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset]) |
           (static_cast<std::uint16_t>(data[offset + 1]) << 8);
}

std::uint32_t readU32(const std::vector<u_char>& data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

float readFloat(const std::vector<u_char>& data, std::size_t offset) {
    const std::uint32_t bits = readU32(data, offset);
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(bits), "32-bit float required");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void writeU16(std::vector<u_char>& data, std::size_t offset, std::uint16_t value) {
    data[offset] = static_cast<u_char>(value & 0xffu);
    data[offset + 1] = static_cast<u_char>((value >> 8) & 0xffu);
}

void appendU32(std::vector<u_char>& data, std::uint32_t value) {
    data.push_back(static_cast<u_char>(value & 0xffu));
    data.push_back(static_cast<u_char>((value >> 8) & 0xffu));
    data.push_back(static_cast<u_char>((value >> 16) & 0xffu));
    data.push_back(static_cast<u_char>((value >> 24) & 0xffu));
}

void appendFloat(std::vector<u_char>& data, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(value) == sizeof(bits), "32-bit float required");
    std::memcpy(&bits, &value, sizeof(value));
    appendU32(data, bits);
}

struct MeshPoint {
    float x = 0.0f;
    float y = 0.0f;
};

MeshPoint sampleMesh(const std::vector<MeshPoint>& mesh,
                     int source_x,
                     int source_y,
                     double gx,
                     double gy) {
    if (mesh.empty() || source_x <= 0 || source_y <= 0) return {};
    if (source_x == 1 || source_y == 1) return mesh.front();

    gx = std::clamp(gx, 0.0, static_cast<double>(source_x - 1));
    gy = std::clamp(gy, 0.0, static_cast<double>(source_y - 1));
    const int x0 = static_cast<int>(std::floor(gx));
    const int y0 = static_cast<int>(std::floor(gy));
    const int x1 = std::min(x0 + 1, source_x - 1);
    const int y1 = std::min(y0 + 1, source_y - 1);
    const double tx = gx - x0;
    const double ty = gy - y0;

    const MeshPoint& a = mesh[static_cast<std::size_t>(y0) * source_x + x0];
    const MeshPoint& b = mesh[static_cast<std::size_t>(y0) * source_x + x1];
    const MeshPoint& c = mesh[static_cast<std::size_t>(y1) * source_x + x0];
    const MeshPoint& d = mesh[static_cast<std::size_t>(y1) * source_x + x1];

    MeshPoint result;
    result.x = static_cast<float>((1.0 - ty) * ((1.0 - tx) * a.x + tx * b.x) +
                                  ty * ((1.0 - tx) * c.x + tx * d.x));
    result.y = static_cast<float>((1.0 - ty) * ((1.0 - tx) * a.y + tx * b.y) +
                                  ty * ((1.0 - tx) * c.y + tx * d.y));
    return result;
}

} // namespace

bool reshapePatchMesh(std::vector<u_char>& codec_packet,
                      bool mesh_enabled,
                      int grid_x,
                      int grid_y,
                      std::string* error) {
    if (error) error->clear();
    if (codec_packet.size() < kCommonHeaderBytes) {
        setError(error, "codec packet is shorter than AFC1 common header");
        return false;
    }

    if (readU32(codec_packet, 0) != kCodecMagic || codec_packet[4] != kCodecVersion) {
        setError(error, "unexpected codec packet magic/version");
        return false;
    }
    if (codec_packet[5] != kPatchType) return true;

    if (codec_packet.size() < kCommonHeaderBytes + kPatchFixedBytes) {
        setError(error, "patch packet is too short");
        return false;
    }

    const std::size_t header_bytes = readU16(codec_packet, 6);
    const int source_x = codec_packet[20];
    const int source_y = codec_packet[21];
    const std::uint16_t flags = readU16(codec_packet, 22);
    if (header_bytes != codec_packet.size() || source_x <= 0 || source_y <= 0 ||
        (flags & ~kPatchKnownFlags) != 0) {
        setError(error, "invalid AFC1 patch header");
        return false;
    }

    const std::size_t mesh_offset = kCommonHeaderBytes + kPatchFixedBytes +
        ((flags & kPatchFlagHomography) ? 2 * sizeof(float) : 0);
    const std::size_t source_points = static_cast<std::size_t>(source_x) * source_y;
    if (mesh_offset + source_points * 2 * sizeof(float) != codec_packet.size()) {
        setError(error, "invalid AFC1 patch mesh size");
        return false;
    }

    int target_x = 1;
    int target_y = 1;
    if (mesh_enabled) {
        if (grid_x < 2 || grid_x > kMaxProductMeshGrid ||
            grid_y < 2 || grid_y > kMaxProductMeshGrid) {
            setError(error, "mesh grid must be in [2, 8]");
            return false;
        }
        target_x = grid_x;
        target_y = grid_y;
    }

    if (mesh_enabled && target_x == source_x && target_y == source_y) return true;

    std::vector<MeshPoint> source_mesh(source_points);
    std::size_t pos = mesh_offset;
    for (MeshPoint& point : source_mesh) {
        point.x = readFloat(codec_packet, pos); pos += sizeof(float);
        point.y = readFloat(codec_packet, pos); pos += sizeof(float);
    }

    std::vector<MeshPoint> target_mesh(static_cast<std::size_t>(target_x) * target_y);
    if (mesh_enabled) {
        for (int y = 0; y < target_y; ++y) {
            const double sy = target_y > 1
                ? static_cast<double>(y) * (source_y - 1) / (target_y - 1)
                : 0.0;
            for (int x = 0; x < target_x; ++x) {
                const double sx = target_x > 1
                    ? static_cast<double>(x) * (source_x - 1) / (target_x - 1)
                    : 0.0;
                target_mesh[static_cast<std::size_t>(y) * target_x + x] =
                    sampleMesh(source_mesh, source_x, source_y, sx, sy);
            }
        }
    }

    std::vector<u_char> rewritten(codec_packet.begin(), codec_packet.begin() + mesh_offset);
    rewritten[20] = static_cast<u_char>(target_x);
    rewritten[21] = static_cast<u_char>(target_y);
    rewritten.reserve(mesh_offset + target_mesh.size() * 2 * sizeof(float));
    for (const MeshPoint& point : target_mesh) {
        appendFloat(rewritten, point.x);
        appendFloat(rewritten, point.y);
    }

    if (rewritten.size() > kMaxCodecPacketBytes || rewritten.size() > 0xffffu) {
        setError(error, "rewritten patch exceeds codec packet limit");
        return false;
    }
    writeU16(rewritten, 6, static_cast<std::uint16_t>(rewritten.size()));
    codec_packet.swap(rewritten);
    return true;
}

} // namespace flowx

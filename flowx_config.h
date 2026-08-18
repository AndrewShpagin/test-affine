#pragma once

#include "flowx_codec.h"

#include <cstdint>
#include <string>

namespace flowx {

enum class SourceType {
    Camera,
    Folder,
    Http,
};

struct SourceConfig {
    SourceType type = SourceType::Camera;
    std::string device = "/dev/video0";
    int width = 0;
    int height = 0;
    double fps = 20.0;

    std::string path;
    bool loop = true;

    std::string url;
    int timeout_ms = 500;
};

struct CodecConfig {
    int keyframe_bytes = 40000;
    int keyframe_period = 5;
    KeyframeCodec keyframe_codec = KeyframeCodec::Jpeg;
    bool strips = true;
    bool homography = true;
    bool mesh = true;
    int mesh_grid_x = 6;
    int mesh_grid_y = 6;
};

struct UdpTargetConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 5000;
};

struct SenderConfig {
    SourceConfig source;
    CodecConfig codec;
    UdpTargetConfig udp;
};

struct UdpListenConfig {
    std::string bind = "0.0.0.0";
    std::uint16_t port = 5000;
};

struct HttpConfig {
    std::string bind = "0.0.0.0";
    std::uint16_t port = 8080;
    int jpeg_quality = 85;
    double stream_fps = 20.0;
    std::string frame_endpoint = "/frame.jpg";
    std::string stream_endpoint = "/stream.mjpg";
    std::string status_endpoint = "/status.json";
};

struct ReceiverConfig {
    UdpListenConfig udp;
    HttpConfig http;
};

const char* sourceTypeName(SourceType type);
const char* keyframeCodecName(KeyframeCodec codec);

SenderConfig loadSenderConfig(const std::string& filename);
ReceiverConfig loadReceiverConfig(const std::string& filename);

} // namespace flowx

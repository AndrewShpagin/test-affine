#include "flowx_config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace flowx {
namespace {

using json = nlohmann::json;

json readJsonFile(const std::string& filename) {
    std::ifstream input(filename);
    if (!input)
        throw std::runtime_error("cannot open config file: " + filename);

    json root;
    try {
        input >> root;
    } catch (const json::exception& e) {
        throw std::runtime_error("invalid JSON in " + filename + ": " + e.what());
    }
    if (!root.is_object())
        throw std::runtime_error("config root must be a JSON object: " + filename);
    return root;
}

const json& objectMember(const json& parent, const char* name) {
    const auto it = parent.find(name);
    if (it == parent.end() || !it->is_object())
        throw std::runtime_error(std::string("missing object: ") + name);
    return *it;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::uint16_t parsePort(const json& object, const char* name, std::uint16_t fallback) {
    const int value = object.value(name, static_cast<int>(fallback));
    if (value <= 0 || value > std::numeric_limits<std::uint16_t>::max())
        throw std::runtime_error(std::string("invalid port: ") + name);
    return static_cast<std::uint16_t>(value);
}

void requirePositive(double value, const char* name) {
    if (!(value > 0.0))
        throw std::runtime_error(std::string(name) + " must be > 0");
}

void requirePositive(int value, const char* name) {
    if (value <= 0)
        throw std::runtime_error(std::string(name) + " must be > 0");
}

void requireRange(int value, int min_value, int max_value, const char* name) {
    if (value < min_value || value > max_value)
        throw std::runtime_error(std::string(name) + " must be in [" +
                                 std::to_string(min_value) + ", " +
                                 std::to_string(max_value) + "]");
}

SourceType parseSourceType(const std::string& value) {
    const std::string type = lower(value);
    if (type == "camera") return SourceType::Camera;
    if (type == "folder") return SourceType::Folder;
    if (type == "http") return SourceType::Http;
    throw std::runtime_error("unknown source.type: " + value);
}

KeyframeCodec parseKeyframeCodec(const std::string& value) {
    const std::string codec = lower(value);
    if (codec == "jpeg" || codec == "jpg") return KeyframeCodec::Jpeg;
    if (codec == "jpeg2000" || codec == "jp2") return KeyframeCodec::Jpeg2000;
    throw std::runtime_error("unknown codec.keyframe_codec: " + value);
}

SourceConfig parseSource(const json& j) {
    SourceConfig cfg;
    cfg.type = parseSourceType(j.value("type", std::string("camera")));
    cfg.fps = j.value("fps", cfg.fps);
    requirePositive(cfg.fps, "source.fps");

    switch (cfg.type) {
    case SourceType::Camera:
        cfg.device = j.value("device", cfg.device);
        cfg.width = j.value("width", cfg.width);
        cfg.height = j.value("height", cfg.height);
        if (cfg.device.empty()) throw std::runtime_error("source.device must not be empty");
        if (cfg.width < 0 || cfg.height < 0)
            throw std::runtime_error("source width/height must be >= 0");
        break;
    case SourceType::Folder:
        cfg.path = j.value("path", std::string());
        cfg.loop = j.value("loop", cfg.loop);
        if (cfg.path.empty()) throw std::runtime_error("source.path is required for folder source");
        break;
    case SourceType::Http:
        cfg.url = j.value("url", std::string());
        cfg.timeout_ms = j.value("timeout_ms", cfg.timeout_ms);
        if (cfg.url.empty()) throw std::runtime_error("source.url is required for http source");
        requirePositive(cfg.timeout_ms, "source.timeout_ms");
        break;
    }
    return cfg;
}

CodecConfig parseCodec(const json& j) {
    CodecConfig cfg;
    cfg.keyframe_bytes = j.value("keyframe_bytes", cfg.keyframe_bytes);
    cfg.keyframe_period = j.value("keyframe_period", cfg.keyframe_period);
    cfg.keyframe_codec = parseKeyframeCodec(
        j.value("keyframe_codec", std::string("jpeg")));
    cfg.grayscale = j.value("grayscale", cfg.grayscale);
    cfg.strips = j.value("strips", cfg.strips);
    cfg.homography = j.value("homography", cfg.homography);
    cfg.mesh = j.value("mesh", cfg.mesh);
    cfg.mesh_grid_x = j.value("mesh_grid_x", cfg.mesh_grid_x);
    cfg.mesh_grid_y = j.value("mesh_grid_y", cfg.mesh_grid_y);
    requirePositive(cfg.keyframe_bytes, "codec.keyframe_bytes");
    requirePositive(cfg.keyframe_period, "codec.keyframe_period");
    requireRange(cfg.mesh_grid_x, 2, 8, "codec.mesh_grid_x");
    requireRange(cfg.mesh_grid_y, 2, 8, "codec.mesh_grid_y");
    return cfg;
}

} // namespace

const char* sourceTypeName(SourceType type) {
    switch (type) {
    case SourceType::Camera: return "camera";
    case SourceType::Folder: return "folder";
    case SourceType::Http: return "http";
    }
    return "unknown";
}

const char* keyframeCodecName(KeyframeCodec codec) {
    return codec == KeyframeCodec::Jpeg2000 ? "jpeg2000" : "jpeg";
}

SenderConfig loadSenderConfig(const std::string& filename) {
    const json root = readJsonFile(filename);
    SenderConfig cfg;
    cfg.source = parseSource(objectMember(root, "source"));
    cfg.codec = parseCodec(objectMember(root, "codec"));

    const json& udp = objectMember(root, "udp");
    cfg.udp.host = udp.value("host", cfg.udp.host);
    cfg.udp.port = parsePort(udp, "port", cfg.udp.port);
    if (cfg.udp.host.empty()) throw std::runtime_error("udp.host must not be empty");
    return cfg;
}

ReceiverConfig loadReceiverConfig(const std::string& filename) {
    const json root = readJsonFile(filename);
    ReceiverConfig cfg;

    const json& udp = objectMember(root, "udp");
    cfg.udp.bind = udp.value("bind", cfg.udp.bind);
    cfg.udp.port = parsePort(udp, "port", cfg.udp.port);
    if (cfg.udp.bind.empty()) throw std::runtime_error("udp.bind must not be empty");

    const json& http = objectMember(root, "http");
    cfg.http.bind = http.value("bind", cfg.http.bind);
    cfg.http.port = parsePort(http, "port", cfg.http.port);
    cfg.http.jpeg_quality = http.value("jpeg_quality", cfg.http.jpeg_quality);
    cfg.http.stream_fps = http.value("stream_fps", cfg.http.stream_fps);
    cfg.http.frame_endpoint = http.value("frame_endpoint", cfg.http.frame_endpoint);
    cfg.http.stream_endpoint = http.value("stream_endpoint", cfg.http.stream_endpoint);
    cfg.http.status_endpoint = http.value("status_endpoint", cfg.http.status_endpoint);

    if (cfg.http.bind.empty()) throw std::runtime_error("http.bind must not be empty");
    if (cfg.http.jpeg_quality < 1 || cfg.http.jpeg_quality > 100)
        throw std::runtime_error("http.jpeg_quality must be in [1, 100]");
    requirePositive(cfg.http.stream_fps, "http.stream_fps");
    if (cfg.http.frame_endpoint.empty() || cfg.http.stream_endpoint.empty() ||
        cfg.http.status_endpoint.empty())
        throw std::runtime_error("HTTP endpoints must not be empty");
    return cfg;
}

} // namespace flowx

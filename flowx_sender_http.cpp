#include "flowx_sender_http.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <cctype>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace flowx {
namespace {

using json = nlohmann::json;

constexpr const char* kSetParamPrefix = "/setparam";

void addNoCacheHeaders(httplib::Response& res) {
    res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    res.set_header("Pragma", "no-cache");
    res.set_header("Access-Control-Allow-Origin", "*");
}

json codecToJson(const CodecConfig& codec) {
    return json{
        {"keyframe_bytes", codec.keyframe_bytes},
        {"keyframe_period", codec.keyframe_period},
        {"keyframe_codec", keyframeCodecName(codec.keyframe_codec)},
        {"grayscale", codec.grayscale},
        {"strips", codec.strips},
        {"homography", codec.homography},
        {"mesh", codec.mesh},
        {"mesh_grid_x", codec.mesh_grid_x},
        {"mesh_grid_y", codec.mesh_grid_y}};
}

// Reads an optional integer field. Returns false with an error if the field is
// present but not an integer.
bool readInt(const json& body, const char* name, int& out, std::string& error) {
    const auto it = body.find(name);
    if (it == body.end()) return true;
    if (!it->is_number_integer() && !it->is_number_unsigned()) {
        error = std::string(name) + " must be an integer";
        return false;
    }
    out = it->get<int>();
    return true;
}

bool readBool(const json& body, const char* name, bool& out, std::string& error) {
    const auto it = body.find(name);
    if (it == body.end()) return true;
    if (!it->is_boolean()) {
        error = std::string(name) + " must be a boolean";
        return false;
    }
    out = it->get<bool>();
    return true;
}

bool readKeyframeCodec(const json& body, KeyframeCodec& out, std::string& error) {
    const auto it = body.find("keyframe_codec");
    if (it == body.end()) return true;
    if (!it->is_string()) {
        error = "keyframe_codec must be a string";
        return false;
    }
    std::string value = it->get<std::string>();
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (value == "jpeg" || value == "jpg") {
        out = KeyframeCodec::Jpeg;
        return true;
    }
    if (value == "jpeg2000" || value == "jp2") {
        if (!cv::haveImageWriter(".jp2")) {
            error = "JPEG2000 writer is not available in this OpenCV build";
            return false;
        }
        out = KeyframeCodec::Jpeg2000;
        return true;
    }
    error = "unknown keyframe_codec: " + value;
    return false;
}

// Applies a JSON patch onto a candidate config and validates it. On success the
// merged config is returned via out; on failure error describes the problem.
bool applyPatch(const CodecConfig& current, const json& body,
                CodecConfig& out, std::string& error) {
    CodecConfig candidate = current;
    if (!readInt(body, "keyframe_bytes", candidate.keyframe_bytes, error) ||
        !readInt(body, "keyframe_period", candidate.keyframe_period, error) ||
        !readBool(body, "grayscale", candidate.grayscale, error) ||
        !readBool(body, "strips", candidate.strips, error) ||
        !readBool(body, "homography", candidate.homography, error) ||
        !readBool(body, "mesh", candidate.mesh, error) ||
        !readInt(body, "mesh_grid_x", candidate.mesh_grid_x, error) ||
        !readInt(body, "mesh_grid_y", candidate.mesh_grid_y, error) ||
        !readKeyframeCodec(body, candidate.keyframe_codec, error)) {
        return false;
    }
    if (!validateCodecConfig(candidate, error)) return false;
    out = candidate;
    return true;
}

// Coerces a string value from the GET path API into the JSON type expected for
// the named field and stores it in patch. Unknown fields and malformed values
// are rejected via error.
bool assignPathField(json& patch, const std::string& name, const std::string& value,
                     std::string& error) {
    static const char* kIntFields[] = {"keyframe_bytes", "keyframe_period",
                                       "mesh_grid_x", "mesh_grid_y"};
    static const char* kBoolFields[] = {"grayscale", "strips", "homography", "mesh"};

    for (const char* field : kIntFields) {
        if (name == field) {
            try {
                std::size_t consumed = 0;
                const int parsed = std::stoi(value, &consumed);
                if (consumed != value.size()) throw std::invalid_argument("trailing characters");
                patch[name] = parsed;
            } catch (const std::exception&) {
                error = name + " must be an integer, got '" + value + "'";
                return false;
            }
            return true;
        }
    }
    for (const char* field : kBoolFields) {
        if (name == field) {
            if (value == "true" || value == "1") patch[name] = true;
            else if (value == "false" || value == "0") patch[name] = false;
            else { error = name + " must be a boolean, got '" + value + "'"; return false; }
            return true;
        }
    }
    if (name == "keyframe_codec") {
        patch[name] = value;
        return true;
    }
    error = "unknown parameter: " + name;
    return false;
}

} // namespace

struct SenderControlServer::Impl {
    SenderControlConfig config;
    CodecParamsStore* params = nullptr;

    httplib::Server server;
    std::thread server_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};

    void registerRoutes() {
        server.Get(config.codec_endpoint, [this](const httplib::Request&, httplib::Response& res) {
            addNoCacheHeaders(res);
            res.set_content(codecToJson(params->snapshot()).dump(2), "application/json");
        });

        const auto update = [this](const httplib::Request& req, httplib::Response& res) {
            addNoCacheHeaders(res);
            json body;
            try {
                body = json::parse(req.body);
            } catch (const json::exception& e) {
                res.status = 400;
                res.set_content(json{{"error", std::string("invalid JSON: ") + e.what()}}.dump(2),
                                "application/json");
                return;
            }
            if (!body.is_object()) {
                res.status = 400;
                res.set_content(json{{"error", "request body must be a JSON object"}}.dump(2),
                                "application/json");
                return;
            }

            CodecConfig merged;
            std::string error;
            if (!applyPatch(params->snapshot(), body, merged, error)) {
                res.status = 400;
                res.set_content(json{{"error", error}}.dump(2), "application/json");
                return;
            }
            params->store(merged);
            res.set_content(codecToJson(merged).dump(2), "application/json");
        };

        server.Post(config.codec_endpoint, update);
        server.Put(config.codec_endpoint, update);

        // Mirror of the JSON patch as a GET path API, e.g.
        // /setparam/keyframe_bytes/5000/grayscale/false
        server.Get(std::string(kSetParamPrefix) + R"(/(.*))",
                   [this](const httplib::Request& req, httplib::Response& res) {
            addNoCacheHeaders(res);

            std::vector<std::string> segments;
            {
                const std::string& rest = req.matches.size() > 1 ? req.matches[1].str()
                                                                  : std::string();
                std::string token;
                std::istringstream stream(rest);
                while (std::getline(stream, token, '/'))
                    if (!token.empty()) segments.push_back(token);
            }

            std::string error;
            if (segments.empty() || segments.size() % 2 != 0) {
                res.status = 400;
                res.set_content(
                    json{{"error", "expected alternating name/value pairs in the path"}}.dump(2),
                    "application/json");
                return;
            }

            json patch = json::object();
            for (std::size_t i = 0; i < segments.size(); i += 2) {
                if (!assignPathField(patch, segments[i], segments[i + 1], error)) {
                    res.status = 400;
                    res.set_content(json{{"error", error}}.dump(2), "application/json");
                    return;
                }
            }

            CodecConfig merged;
            if (!applyPatch(params->snapshot(), patch, merged, error)) {
                res.status = 400;
                res.set_content(json{{"error", error}}.dump(2), "application/json");
                return;
            }
            params->store(merged);
            res.set_content(codecToJson(merged).dump(2), "application/json");
        });
    }
};

SenderControlServer::SenderControlServer() : impl_(std::make_unique<Impl>()) {}
SenderControlServer::~SenderControlServer() { stop(); }
SenderControlServer::SenderControlServer(SenderControlServer&&) noexcept = default;
SenderControlServer& SenderControlServer::operator=(SenderControlServer&&) noexcept = default;

bool SenderControlServer::start(const SenderControlConfig& config,
                                CodecParamsStore& params,
                                std::string& error) {
    error.clear();
    if (!impl_) impl_ = std::make_unique<Impl>();
    if (impl_->running.load(std::memory_order_relaxed)) {
        error = "sender control server is already running";
        return false;
    }
    if (config.codec_endpoint.empty() || config.codec_endpoint.front() != '/') {
        error = "control.codec_endpoint must begin with '/'";
        return false;
    }

    impl_->config = config;
    impl_->params = &params;
    impl_->stopping.store(false, std::memory_order_relaxed);
    impl_->registerRoutes();

    if (!impl_->server.bind_to_port(config.bind, config.port)) {
        error = "cannot bind control server to " + config.bind + ":" + std::to_string(config.port);
        return false;
    }
    impl_->running.store(true, std::memory_order_relaxed);
    impl_->server_thread = std::thread([this] {
        impl_->server.listen_after_bind();
        impl_->running.store(false, std::memory_order_relaxed);
    });
    return true;
}

void SenderControlServer::stop() {
    if (!impl_) return;
    impl_->stopping.store(true, std::memory_order_relaxed);
    impl_->server.stop();
    if (impl_->server_thread.joinable()) impl_->server_thread.join();
    impl_->running.store(false, std::memory_order_relaxed);
}

bool SenderControlServer::isRunning() const {
    return impl_ && impl_->running.load(std::memory_order_relaxed);
}

} // namespace flowx

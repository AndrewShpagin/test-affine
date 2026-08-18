#include "flowx_http_server.h"

#include "flowx_protocol.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace flowx {
namespace {

using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

constexpr const char* kMjpegBoundary = "flowxframe";

std::uint64_t systemTimestampUs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

bool validEndpoint(const std::string& endpoint) {
    return !endpoint.empty() && endpoint.front() == '/';
}

void addNoCacheHeaders(httplib::Response& res) {
    res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    res.set_header("Pragma", "no-cache");
    res.set_header("Access-Control-Allow-Origin", "*");
}

} // namespace

struct HttpServer::Impl {
    HttpConfig config;
    const FrameStore* frames = nullptr;
    const ReceiverStatusStore* status = nullptr;

    httplib::Server server;
    std::thread server_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};

    std::mutex jpeg_mutex;
    std::uint64_t cached_sequence = 0;
    std::shared_ptr<const std::vector<unsigned char>> cached_jpeg;
    std::atomic<std::uint64_t> jpeg_encodes{0};
    std::atomic<std::uint64_t> jpeg_encode_failures{0};

    bool getJpeg(const std::shared_ptr<const PublishedFrame>& frame,
                 std::shared_ptr<const std::vector<unsigned char>>& jpeg) {
        if (!frame || frame->image.empty()) return false;

        std::lock_guard<std::mutex> lock(jpeg_mutex);
        if (cached_jpeg && cached_sequence == frame->metadata.sequence) {
            jpeg = cached_jpeg;
            return true;
        }

        std::vector<unsigned char> encoded;
        const std::vector<int> params{
            cv::IMWRITE_JPEG_QUALITY, config.jpeg_quality
        };
        if (!cv::imencode(".jpg", frame->image, encoded, params) || encoded.empty()) {
            jpeg_encode_failures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        cached_sequence = frame->metadata.sequence;
        cached_jpeg = std::make_shared<const std::vector<unsigned char>>(std::move(encoded));
        jpeg = cached_jpeg;
        jpeg_encodes.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::string statusJson() const {
        const ReceiverStatus s = status->snapshot();
        const auto latest = frames->latest();
        const std::uint64_t now_us = systemTimestampUs();

        json root;
        root["flowx_version"] = kProtocolVersion;
        root["active_stream_id"] = s.active_stream_id;
        root["uptime_ms"] = s.started_timestamp_us > 0 && now_us >= s.started_timestamp_us
            ? static_cast<double>(now_us - s.started_timestamp_us) / 1000.0
            : 0.0;

        root["udp"] = {
            {"datagrams", s.received_datagrams},
            {"bytes", s.received_bytes},
            {"invalid", s.invalid_datagrams},
            {"ignored", s.ignored_datagrams},
            {"other_stream", s.ignored_other_stream},
            {"stale_frames", s.stale_frames},
            {"stream_resets", s.stream_resets}
        };

        root["decode"] = {
            {"frames", s.decoded_frames},
            {"keyframes", s.decoded_keyframes},
            {"patches", s.decoded_patches}
        };

        root["http"] = {
            {"jpeg_quality", config.jpeg_quality},
            {"stream_fps", config.stream_fps},
            {"jpeg_encodes", jpeg_encodes.load(std::memory_order_relaxed)},
            {"jpeg_encode_failures", jpeg_encode_failures.load(std::memory_order_relaxed)},
            {"frame_endpoint", config.frame_endpoint},
            {"stream_endpoint", config.stream_endpoint},
            {"status_endpoint", config.status_endpoint}
        };

        if (!latest) {
            root["frame"] = nullptr;
        } else {
            json frame = {
                {"sequence", latest->metadata.sequence},
                {"stream_id", latest->metadata.stream_id},
                {"frame_id", latest->metadata.frame_id},
                {"keyframe_id", latest->metadata.keyframe_id},
                {"keyframe", latest->metadata.keyframe},
                {"width", latest->image.cols},
                {"height", latest->image.rows},
                {"capture_timestamp_us", latest->metadata.capture_timestamp_us},
                {"receive_timestamp_us", latest->metadata.receive_timestamp_us}
            };
            if (latest->metadata.receive_timestamp_us > 0 &&
                now_us >= latest->metadata.receive_timestamp_us) {
                frame["receive_age_ms"] =
                    static_cast<double>(now_us - latest->metadata.receive_timestamp_us) / 1000.0;
            } else {
                frame["receive_age_ms"] = nullptr;
            }
            root["frame"] = std::move(frame);
        }

        return root.dump(2);
    }

    void registerRoutes() {
        server.Get(config.frame_endpoint, [this](const httplib::Request&, httplib::Response& res) {
            addNoCacheHeaders(res);
            const auto frame = frames->latest();
            if (!frame) {
                res.status = 503;
                res.set_content("No decoded frame available yet\n", "text/plain");
                return;
            }

            std::shared_ptr<const std::vector<unsigned char>> jpeg;
            if (!getJpeg(frame, jpeg)) {
                res.status = 500;
                res.set_content("JPEG encoding failed\n", "text/plain");
                return;
            }

            res.set_header("Content-Type", "image/jpeg");
            res.set_header("X-FlowX-Stream-Id", std::to_string(frame->metadata.stream_id));
            res.set_header("X-FlowX-Frame-Id", std::to_string(frame->metadata.frame_id));
            res.body.assign(reinterpret_cast<const char*>(jpeg->data()), jpeg->size());
        });

        server.Get(config.status_endpoint, [this](const httplib::Request&, httplib::Response& res) {
            addNoCacheHeaders(res);
            res.set_content(statusJson(), "application/json");
        });

        server.Get(config.stream_endpoint, [this](const httplib::Request&, httplib::Response& res) {
            addNoCacheHeaders(res);

            struct StreamState {
                std::uint64_t last_sequence = 0;
                Clock::time_point next_due{};
            };
            auto state = std::make_shared<StreamState>();
            const auto frame_period = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1.0 / config.stream_fps));

            const std::string content_type =
                std::string("multipart/x-mixed-replace; boundary=") + kMjpegBoundary;

            res.set_chunked_content_provider(
                content_type,
                [this, state, frame_period](std::size_t, httplib::DataSink& sink) {
                    if (stopping.load(std::memory_order_relaxed)) return false;

                    const auto now = Clock::now();
                    if (state->next_due != Clock::time_point{} && now < state->next_due)
                        std::this_thread::sleep_until(state->next_due);
                    if (stopping.load(std::memory_order_relaxed)) return false;

                    std::shared_ptr<const PublishedFrame> frame = frames->latest();
                    if (!frame || frame->metadata.sequence <= state->last_sequence) {
                        frame.reset();
                        frames->waitForNext(state->last_sequence, frame,
                                            std::chrono::milliseconds(500));
                    }
                    if (!frame) return !stopping.load(std::memory_order_relaxed);
                    if (frame->metadata.sequence <= state->last_sequence) return true;

                    std::shared_ptr<const std::vector<unsigned char>> jpeg;
                    if (!getJpeg(frame, jpeg)) return true;

                    std::ostringstream header;
                    header << "--" << kMjpegBoundary << "\r\n"
                           << "Content-Type: image/jpeg\r\n"
                           << "Content-Length: " << jpeg->size() << "\r\n"
                           << "X-FlowX-Stream-Id: " << frame->metadata.stream_id << "\r\n"
                           << "X-FlowX-Frame-Id: " << frame->metadata.frame_id << "\r\n\r\n";
                    const std::string head = header.str();

                    if (!sink.write(head.data(), head.size())) return false;
                    if (!sink.write(reinterpret_cast<const char*>(jpeg->data()), jpeg->size()))
                        return false;
                    if (!sink.write("\r\n", 2)) return false;

                    state->last_sequence = frame->metadata.sequence;
                    state->next_due = Clock::now() + frame_period;
                    return true;
                });
        });
    }
};

HttpServer::HttpServer() : impl_(std::make_unique<Impl>()) {}
HttpServer::~HttpServer() { stop(); }
HttpServer::HttpServer(HttpServer&&) noexcept = default;
HttpServer& HttpServer::operator=(HttpServer&&) noexcept = default;

bool HttpServer::start(const HttpConfig& config,
                       const FrameStore& frames,
                       const ReceiverStatusStore& status,
                       std::string& error) {
    error.clear();
    if (!impl_) impl_ = std::make_unique<Impl>();
    if (impl_->running.load(std::memory_order_relaxed)) {
        error = "HTTP server is already running";
        return false;
    }

    if (!validEndpoint(config.frame_endpoint) ||
        !validEndpoint(config.stream_endpoint) ||
        !validEndpoint(config.status_endpoint)) {
        error = "HTTP endpoint paths must begin with '/'";
        return false;
    }
    if (config.frame_endpoint == config.stream_endpoint ||
        config.frame_endpoint == config.status_endpoint ||
        config.stream_endpoint == config.status_endpoint) {
        error = "HTTP endpoint paths must be unique";
        return false;
    }

    impl_->config = config;
    impl_->frames = &frames;
    impl_->status = &status;
    impl_->stopping.store(false, std::memory_order_relaxed);
    impl_->registerRoutes();

    if (!impl_->server.bind_to_port(config.bind, config.port)) {
        error = "cannot bind HTTP server to " + config.bind + ":" +
                std::to_string(config.port);
        return false;
    }

    impl_->running.store(true, std::memory_order_relaxed);
    impl_->server_thread = std::thread([this] {
        impl_->server.listen_after_bind();
        impl_->running.store(false, std::memory_order_relaxed);
    });
    return true;
}

void HttpServer::stop() {
    if (!impl_) return;
    impl_->stopping.store(true, std::memory_order_relaxed);
    impl_->server.stop();
    if (impl_->server_thread.joinable()) impl_->server_thread.join();
    impl_->running.store(false, std::memory_order_relaxed);
}

bool HttpServer::isRunning() const {
    return impl_ && impl_->running.load(std::memory_order_relaxed);
}

} // namespace flowx

#include "flowx_image_source.h"

#include <httplib.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace flowx {
namespace {

namespace fs = std::filesystem;
using SteadyClock = std::chrono::steady_clock;

class FramePacer {
public:
    explicit FramePacer(double fps)
        : period_(std::chrono::duration_cast<SteadyClock::duration>(
              std::chrono::duration<double>(1.0 / fps))) {}

    void reset() { next_ = SteadyClock::time_point{}; }

    void wait() {
        const auto now = SteadyClock::now();
        if (next_ == SteadyClock::time_point{}) next_ = now;
        if (next_ > now) std::this_thread::sleep_until(next_);
        const auto after_wait = SteadyClock::now();
        next_ += period_;
        if (next_ < after_wait - period_) next_ = after_wait + period_;
    }

private:
    SteadyClock::duration period_;
    SteadyClock::time_point next_{};
};

bool isImageFile(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
           ext == ".bmp" || ext == ".tif" || ext == ".tiff" || ext == ".webp";
}

bool parseCameraIndex(const std::string& device, int& index) {
    if (device.empty() || !std::all_of(device.begin(), device.end(),
        [](unsigned char c) { return std::isdigit(c) != 0; })) return false;
    try {
        index = std::stoi(device);
        return index >= 0;
    } catch (...) {
        return false;
    }
}

struct ParsedHttpUrl {
    std::string host;
    int port = 80;
    std::string path = "/";
};

bool parseHttpUrl(const std::string& url, ParsedHttpUrl& out, std::string& error) {
    constexpr const char* prefix = "http://";
    if (url.rfind(prefix, 0) != 0) {
        error = "HTTP source currently supports http:// URLs only: " + url;
        return false;
    }

    const std::size_t begin = 7;
    const std::size_t slash = url.find('/', begin);
    const std::string authority = url.substr(
        begin, slash == std::string::npos ? std::string::npos : slash - begin);
    out.path = slash == std::string::npos ? "/" : url.substr(slash);
    if (authority.empty()) {
        error = "HTTP source URL has no host";
        return false;
    }

    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) {
            error = "invalid bracketed IPv6 HTTP URL";
            return false;
        }
        out.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                error = "invalid HTTP URL port";
                return false;
            }
            try { out.port = std::stoi(authority.substr(close + 2)); }
            catch (...) { error = "invalid HTTP URL port"; return false; }
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            out.host = authority.substr(0, colon);
            try { out.port = std::stoi(authority.substr(colon + 1)); }
            catch (...) { error = "invalid HTTP URL port"; return false; }
        } else {
            out.host = authority;
        }
    }

    if (out.host.empty() || out.port <= 0 || out.port > 65535) {
        error = "invalid HTTP source host/port";
        return false;
    }
    return true;
}

void setClientTimeout(httplib::Client& client, int timeout_ms) {
    const time_t sec = timeout_ms / 1000;
    const time_t usec = static_cast<time_t>(timeout_ms % 1000) * 1000;
    client.set_connection_timeout(sec, usec);
    client.set_read_timeout(sec, usec);
    client.set_write_timeout(sec, usec);
}

class CameraSource final : public ImageSource {
public:
    explicit CameraSource(SourceConfig config) : config_(std::move(config)) {}

    bool open(std::string& error) override {
        error.clear();
        int index = -1;
        const bool ok = parseCameraIndex(config_.device, index)
            ? capture_.open(index)
            : capture_.open(config_.device);
        if (!ok) {
            error = "cannot open camera: " + config_.device;
            return false;
        }
        if (config_.width > 0) capture_.set(cv::CAP_PROP_FRAME_WIDTH, config_.width);
        if (config_.height > 0) capture_.set(cv::CAP_PROP_FRAME_HEIGHT, config_.height);
        capture_.set(cv::CAP_PROP_FPS, config_.fps);
        return true;
    }

    SourceReadResult read(CapturedFrame& frame, std::string& error) override {
        error.clear();
        cv::Mat image;
        if (!capture_.read(image) || image.empty()) {
            error = "camera read failed";
            return SourceReadResult::Retry;
        }
        frame.image = std::move(image);
        frame.capture_timestamp_us = systemTimestampUs();
        return SourceReadResult::Frame;
    }

    const char* name() const override { return "camera"; }

private:
    SourceConfig config_;
    cv::VideoCapture capture_;
};

class FolderSource final : public ImageSource {
public:
    explicit FolderSource(SourceConfig config)
        : config_(std::move(config)), pacer_(config_.fps) {}

    bool open(std::string& error) override {
        error.clear();
        std::error_code ec;
        if (!fs::is_directory(config_.path, ec)) {
            error = "folder source path is not a directory: " + config_.path;
            return false;
        }
        files_.clear();
        for (const auto& entry : fs::directory_iterator(config_.path, ec)) {
            if (ec) break;
            if (entry.is_regular_file() && isImageFile(entry.path()))
                files_.push_back(entry.path());
        }
        if (ec) {
            error = "cannot enumerate folder source: " + ec.message();
            return false;
        }
        std::sort(files_.begin(), files_.end());
        if (files_.empty()) {
            error = "folder source contains no images: " + config_.path;
            return false;
        }
        index_ = 0;
        pacer_.reset();
        return true;
    }

    SourceReadResult read(CapturedFrame& frame, std::string& error) override {
        error.clear();
        if (files_.empty()) return SourceReadResult::End;
        if (index_ >= files_.size()) {
            if (!config_.loop) return SourceReadResult::End;
            index_ = 0;
        }

        pacer_.wait();
        const fs::path path = files_[index_++];
        cv::Mat image = cv::imread(path.string(), cv::IMREAD_COLOR);
        if (image.empty()) {
            error = "cannot read image: " + path.string();
            return SourceReadResult::Retry;
        }
        frame.image = std::move(image);
        frame.capture_timestamp_us = systemTimestampUs();
        return SourceReadResult::Frame;
    }

    const char* name() const override { return "folder"; }

private:
    SourceConfig config_;
    FramePacer pacer_;
    std::vector<fs::path> files_;
    std::size_t index_ = 0;
};

class HttpSource final : public ImageSource {
public:
    explicit HttpSource(SourceConfig config)
        : config_(std::move(config)), pacer_(config_.fps) {}

    bool open(std::string& error) override {
        error.clear();
        ParsedHttpUrl parsed;
        if (!parseHttpUrl(config_.url, parsed, error)) return false;
        path_ = std::move(parsed.path);
        client_ = std::make_unique<httplib::Client>(parsed.host, parsed.port);
        setClientTimeout(*client_, config_.timeout_ms);
        pacer_.reset();
        return true;
    }

    SourceReadResult read(CapturedFrame& frame, std::string& error) override {
        error.clear();
        if (!client_) {
            error = "HTTP source is not open";
            return SourceReadResult::End;
        }

        pacer_.wait();
        auto response = client_->Get(path_.c_str());
        if (!response) {
            error = "HTTP GET failed, error=" +
                    std::to_string(static_cast<int>(response.error()));
            return SourceReadResult::Retry;
        }
        if (response->status != 200) {
            error = "HTTP GET returned status " + std::to_string(response->status);
            return SourceReadResult::Retry;
        }
        if (response->body.empty()) {
            error = "HTTP GET returned an empty body";
            return SourceReadResult::Retry;
        }

        std::vector<unsigned char> bytes(response->body.begin(), response->body.end());
        cv::Mat image = cv::imdecode(bytes, cv::IMREAD_COLOR);
        if (image.empty()) {
            error = "HTTP response is not a decodable image";
            return SourceReadResult::Retry;
        }
        frame.image = std::move(image);
        frame.capture_timestamp_us = systemTimestampUs();
        return SourceReadResult::Frame;
    }

    const char* name() const override { return "http"; }

private:
    SourceConfig config_;
    FramePacer pacer_;
    std::unique_ptr<httplib::Client> client_;
    std::string path_;
};

} // namespace

std::uint64_t systemTimestampUs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::unique_ptr<ImageSource> createImageSource(const SourceConfig& config) {
    switch (config.type) {
    case SourceType::Camera: return std::make_unique<CameraSource>(config);
    case SourceType::Folder: return std::make_unique<FolderSource>(config);
    case SourceType::Http: return std::make_unique<HttpSource>(config);
    }
    return {};
}

} // namespace flowx

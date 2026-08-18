#pragma once

#include "flowx_config.h"

#include <opencv2/core.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace flowx {

struct CapturedFrame {
    cv::Mat image;
    std::uint64_t capture_timestamp_us = 0;
};

enum class SourceReadResult {
    Frame,
    Retry,
    End,
};

class ImageSource {
public:
    virtual ~ImageSource() = default;

    virtual bool open(std::string& error) = 0;
    virtual SourceReadResult read(CapturedFrame& frame, std::string& error) = 0;
    virtual const char* name() const = 0;
};

std::unique_ptr<ImageSource> createImageSource(const SourceConfig& config);
std::uint64_t systemTimestampUs();

} // namespace flowx

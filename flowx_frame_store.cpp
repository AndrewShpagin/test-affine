#include "flowx_frame_store.h"

#include <utility>

namespace flowx {

void FrameStore::publish(cv::Mat image, FrameMetadata metadata) {
    if (image.empty()) return;

    auto frame = std::make_shared<PublishedFrame>();
    frame->image = std::move(image);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        metadata.sequence = next_sequence_++;
        frame->metadata = metadata;
        latest_ = frame;
    }
    changed_.notify_all();
}

std::shared_ptr<const PublishedFrame> FrameStore::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

bool FrameStore::waitForNext(std::uint64_t after_sequence,
                             std::shared_ptr<const PublishedFrame>& frame,
                             std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ready = changed_.wait_for(lock, timeout, [&] {
        return latest_ && latest_->metadata.sequence > after_sequence;
    });
    if (!ready) return false;
    frame = latest_;
    return true;
}

} // namespace flowx

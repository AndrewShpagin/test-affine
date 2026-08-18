#include "flowx_config.h"
#include "flowx_frame_store.h"
#include "flowx_http_server.h"
#include "flowx_protocol.h"
#include "flowx_raw_store.h"
#include "flowx_receiver_status.h"
#include "flowx_udp_receiver.h"

#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

void signalHandler(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

std::uint64_t systemTimestampUs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

bool frameIdNewer(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::int32_t>(a - b) > 0;
}

struct FrameArrival {
    std::uint64_t capture_timestamp_us = 0;
    std::uint64_t receive_timestamp_us = 0;
};

void printUsage() {
    std::cerr << "Usage: flowx_receiver [config.json] [--dump-last FILE]\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string config_file = "config/flowx_receiver.json";
    std::string dump_last_file;

    int arg = 1;
    if (arg < argc && std::string(argv[arg]) != "--dump-last")
        config_file = argv[arg++];
    if (arg < argc) {
        if (std::string(argv[arg]) != "--dump-last" || arg + 1 >= argc) {
            printUsage();
            return 2;
        }
        dump_last_file = argv[arg + 1];
        arg += 2;
    }
    if (arg != argc) {
        printUsage();
        return 2;
    }

    try {
        const flowx::ReceiverConfig cfg = flowx::loadReceiverConfig(config_file);

        flowx::UdpReceiver udp;
        std::string error;
        if (!udp.open(cfg.udp, error))
            throw std::runtime_error("UDP open failed: " + error);

        auto decoder = std::make_unique<flowx::Decoder>();
        decoder->setReusePreviousFrameBorders(true);
        std::vector<flowx::u_char> current_jpeg;
        flowx::FrameStore frame_store;
        flowx::RawFrameStore raw_frame_store;

        std::uint32_t active_stream_id = 0;
        std::unordered_set<std::uint32_t> retired_streams;
        std::deque<std::uint32_t> retired_stream_order;

        std::unordered_map<std::uint32_t, FrameArrival> frame_arrivals;
        std::deque<std::uint32_t> frame_arrival_order;
        constexpr std::size_t kMaxRememberedFrames = 1024;
        constexpr std::size_t kMaxRetiredStreams = 8;

        bool have_published_frame = false;
        std::uint32_t last_published_frame_id = 0;

        auto clearDecoderState = [&] {
            decoder = std::make_unique<flowx::Decoder>();
            decoder->setReusePreviousFrameBorders(true);
            current_jpeg.clear();
            frame_arrivals.clear();
            frame_arrival_order.clear();
            have_published_frame = false;
            last_published_frame_id = 0;
        };

        auto retireStream = [&](std::uint32_t stream_id) {
            if (stream_id == 0 || retired_streams.count(stream_id)) return;
            retired_streams.insert(stream_id);
            retired_stream_order.push_back(stream_id);
            while (retired_stream_order.size() > kMaxRetiredStreams) {
                retired_streams.erase(retired_stream_order.front());
                retired_stream_order.pop_front();
            }
        };

        auto rememberArrival = [&](const flowx::PacketMetadata& metadata,
                                   std::uint64_t receive_timestamp_us) {
            auto it = frame_arrivals.find(metadata.frame_id);
            if (it == frame_arrivals.end()) {
                frame_arrivals.emplace(metadata.frame_id,
                    FrameArrival{metadata.capture_timestamp_us, receive_timestamp_us});
                frame_arrival_order.push_back(metadata.frame_id);
                while (frame_arrival_order.size() > kMaxRememberedFrames) {
                    const std::uint32_t old = frame_arrival_order.front();
                    frame_arrival_order.pop_front();
                    frame_arrivals.erase(old);
                }
            }
        };

        auto takeArrival = [&](std::uint32_t frame_id,
                               const flowx::PacketMetadata& fallback,
                               std::uint64_t fallback_receive_us) {
            FrameArrival arrival{fallback.capture_timestamp_us, fallback_receive_us};
            const auto it = frame_arrivals.find(frame_id);
            if (it != frame_arrivals.end()) {
                arrival = it->second;
                frame_arrivals.erase(it);
            }
            return arrival;
        };

        std::uint64_t received_datagrams = 0;
        std::uint64_t received_bytes = 0;
        std::uint64_t invalid_datagrams = 0;
        std::uint64_t ignored_datagrams = 0;
        std::uint64_t ignored_other_stream = 0;
        std::uint64_t stale_frames = 0;
        std::uint64_t stream_resets = 0;
        std::uint64_t decoded_frames = 0;
        std::uint64_t decoded_keyframes = 0;
        std::uint64_t decoded_patches = 0;

        const std::uint64_t started_timestamp_us = systemTimestampUs();
        flowx::ReceiverStatusStore status_store;
        auto publishStatus = [&] {
            flowx::ReceiverStatus status;
            status.started_timestamp_us = started_timestamp_us;
            status.active_stream_id = active_stream_id;
            status.received_datagrams = received_datagrams;
            status.received_bytes = received_bytes;
            status.invalid_datagrams = invalid_datagrams;
            status.ignored_datagrams = ignored_datagrams;
            status.ignored_other_stream = ignored_other_stream;
            status.stale_frames = stale_frames;
            status.stream_resets = stream_resets;
            status.decoded_frames = decoded_frames;
            status.decoded_keyframes = decoded_keyframes;
            status.decoded_patches = decoded_patches;
            status_store.publish(status);
        };
        publishStatus();

        flowx::HttpServer http_server;
        if (!http_server.start(cfg.http, frame_store, raw_frame_store, status_store, error))
            throw std::runtime_error("HTTP server start failed: " + error);

        std::cout << "FlowX receiver\n"
                  << "  config: " << config_file << '\n'
                  << "  UDP listen: " << cfg.udp.bind << ':' << cfg.udp.port << '\n'
                  << "  HTTP listen: " << cfg.http.bind << ':' << cfg.http.port << '\n'
                  << "  frame:  " << cfg.http.frame_endpoint << '\n'
                  << "  stream: " << cfg.http.stream_endpoint
                  << " @ " << cfg.http.stream_fps << " fps\n"
                  << "  status: " << cfg.http.status_endpoint << '\n'
                  << "  browser: /flowx.html  raw: /flowx.bin\n"
                  << "  HTTP JPEG quality: " << cfg.http.jpeg_quality << '\n'
                  << "  FlowX wire: v" << static_cast<int>(flowx::kProtocolVersion)
                  << ", max datagram=" << flowx::kMaxUdpDatagramBytes << " B\n"
                  << "  decoder border reuse: yes\n";
        if (!dump_last_file.empty())
            std::cout << "  debug dump on exit: " << dump_last_file << '\n';

        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);

        auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool fatal_error = false;

        while (!g_stop.load(std::memory_order_relaxed)) {
            std::vector<flowx::u_char> datagram;
            const flowx::UdpReceiveResult receive_result = udp.receive(datagram, 250, error);
            if (receive_result == flowx::UdpReceiveResult::Timeout) continue;
            if (receive_result == flowx::UdpReceiveResult::Error) {
                if (g_stop.load(std::memory_order_relaxed)) break;
                std::cerr << "UDP receive failed: " << error << '\n';
                fatal_error = true;
                break;
            }
            if (receive_result == flowx::UdpReceiveResult::Ignored) {
                ++ignored_datagrams;
                publishStatus();
                continue;
            }

            ++received_datagrams;
            received_bytes += datagram.size();
            const std::uint64_t receive_timestamp_us = systemTimestampUs();

            flowx::FlowXPacket packet;
            if (!flowx::unwrapCodecPacket(datagram, packet, &error)) {
                ++invalid_datagrams;
                publishStatus();
                continue;
            }

            const flowx::PacketMetadata& metadata = packet.metadata;

            if (active_stream_id == 0) {
                if (metadata.frame_id != metadata.keyframe_id) {
                    ++ignored_other_stream;
                    publishStatus();
                    continue;
                }
                active_stream_id = metadata.stream_id;
                clearDecoderState();
                std::cout << "FlowX stream started: " << active_stream_id << '\n';
            } else if (metadata.stream_id != active_stream_id) {
                if (retired_streams.count(metadata.stream_id)) {
                    ++ignored_other_stream;
                    publishStatus();
                    continue;
                }
                if (metadata.frame_id != metadata.keyframe_id) {
                    ++ignored_other_stream;
                    publishStatus();
                    continue;
                }

                retireStream(active_stream_id);
                active_stream_id = metadata.stream_id;
                clearDecoderState();
                ++stream_resets;
                std::cout << "FlowX stream changed, decoder reset: "
                          << active_stream_id << '\n';
            }

            // Publish the exact validated FlowX datagram to the browser transport.
            // RawFrameStore groups all datagrams of one frame into an atomic bundle,
            // so a slow HTTP client skips old frames rather than half a keyframe.
            raw_frame_store.push(datagram, metadata);

            rememberArrival(metadata, receive_timestamp_us);
            decoder->pushData(packet.codec_packet);

            std::vector<flowx::u_char> new_jpeg;
            if (decoder->updateKeyframe(new_jpeg)) {
                current_jpeg = std::move(new_jpeg);

                cv::Mat image;
                decoder->render(image, {}, current_jpeg);
                if (!image.empty()) {
                    const std::uint32_t frame_id = decoder->keyframeId();
                    const FrameArrival arrival = takeArrival(
                        frame_id, metadata, receive_timestamp_us);

                    flowx::FrameMetadata published;
                    published.stream_id = active_stream_id;
                    published.frame_id = frame_id;
                    published.keyframe_id = frame_id;
                    published.capture_timestamp_us = arrival.capture_timestamp_us;
                    published.receive_timestamp_us = arrival.receive_timestamp_us;
                    published.keyframe = true;
                    frame_store.publish(std::move(image), published);
                    last_published_frame_id = frame_id;
                    have_published_frame = true;
                    ++decoded_frames;
                    ++decoded_keyframes;
                }
            }

            std::vector<flowx::PatchData> patch;
            while (decoder->getNextPatch(patch)) {
                if (patch.empty()) continue;

                const flowx::PatchData& last = patch.back();
                const FrameArrival arrival = takeArrival(
                    last.frame_id, metadata, receive_timestamp_us);

                if (have_published_frame &&
                    !frameIdNewer(last.frame_id, last_published_frame_id)) {
                    ++stale_frames;
                    continue;
                }

                cv::Mat image;
                decoder->render(image, patch, current_jpeg);
                if (image.empty()) continue;

                flowx::FrameMetadata published;
                published.stream_id = active_stream_id;
                published.frame_id = last.frame_id;
                published.keyframe_id = last.keyframe_id;
                published.capture_timestamp_us = arrival.capture_timestamp_us;
                published.receive_timestamp_us = arrival.receive_timestamp_us;
                published.keyframe = false;
                frame_store.publish(std::move(image), published);
                last_published_frame_id = last.frame_id;
                have_published_frame = true;
                ++decoded_frames;
                ++decoded_patches;
            }

            publishStatus();

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_report) {
                const auto latest = frame_store.latest();
                std::cout << "udp=" << received_datagrams
                          << " bytes=" << received_bytes
                          << " invalid=" << invalid_datagrams
                          << " ignored=" << ignored_datagrams
                          << " other-stream=" << ignored_other_stream
                          << " stale=" << stale_frames
                          << " resets=" << stream_resets
                          << " decoded=" << decoded_frames
                          << " (key=" << decoded_keyframes
                          << " patch=" << decoded_patches << ')';
                if (latest) {
                    std::cout << " latest=" << latest->metadata.frame_id
                              << " " << latest->image.cols << 'x' << latest->image.rows;
                }
                std::cout << '\n';
                next_report = now + std::chrono::seconds(2);
            }
        }

        publishStatus();
        raw_frame_store.close();
        http_server.stop();

        if (!dump_last_file.empty()) {
            const auto latest = frame_store.latest();
            if (!latest || latest->image.empty()) {
                std::cerr << "No decoded frame available for --dump-last\n";
            } else {
                const std::filesystem::path output_path(dump_last_file);
                if (!output_path.parent_path().empty()) {
                    std::error_code ec;
                    std::filesystem::create_directories(output_path.parent_path(), ec);
                    if (ec)
                        std::cerr << "Cannot create debug output directory: "
                                  << ec.message() << '\n';
                }
                if (cv::imwrite(output_path.string(), latest->image))
                    std::cout << "Saved last decoded frame: " << output_path.string() << '\n';
                else
                    std::cerr << "Failed to save last decoded frame: "
                              << output_path.string() << '\n';
            }
        }

        std::cout << "FlowX receiver stopped: udp=" << received_datagrams
                  << " decoded=" << decoded_frames
                  << " invalid=" << invalid_datagrams
                  << " ignored=" << (ignored_datagrams + ignored_other_stream)
                  << " stale=" << stale_frames
                  << '\n';
        return fatal_error ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "flowx_receiver: " << e.what() << '\n';
        return 1;
    }
}

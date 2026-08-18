#include "flowx_config.h"
#include "flowx_image_source.h"
#include "flowx_latest_frame.h"
#include "flowx_net.h"
#include "flowx_protocol.h"

#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

void signalHandler(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "Usage: flowx_sender [config.json]\n";
        return 2;
    }

    const std::string config_file = argc > 1 ? argv[1] : "config/flowx_sender.json";

    try {
        const flowx::SenderConfig cfg = flowx::loadSenderConfig(config_file);
        const std::uint32_t stream_id = flowx::generateStreamId();

        if (cfg.codec.keyframe_codec == flowx::KeyframeCodec::Jpeg2000 &&
            !cv::haveImageWriter(".jp2")) {
            throw std::runtime_error("JPEG2000 writer is not available in this OpenCV build");
        }

        std::unique_ptr<flowx::ImageSource> source = flowx::createImageSource(cfg.source);
        if (!source) throw std::runtime_error("cannot create image source");

        std::string error;
        if (!source->open(error))
            throw std::runtime_error("source open failed: " + error);

        flowx::UdpSender udp;
        if (!udp.open(cfg.udp, error))
            throw std::runtime_error("UDP open failed: " + error);

        flowx::Encoder encoder;
        encoder.setStripsKeyframes(cfg.codec.strips);
        encoder.setKeyframeCodec(cfg.codec.keyframe_codec);
        encoder.setHomographyTransform(cfg.codec.homography);

        std::cout << "FlowX sender\n"
                  << "  config: " << config_file << '\n'
                  << "  stream id: " << stream_id << '\n'
                  << "  source: " << source->name() << " @ " << cfg.source.fps << " fps\n"
                  << "  codec: " << flowx::keyframeCodecName(cfg.codec.keyframe_codec)
                  << ", key=" << cfg.codec.keyframe_bytes << " B/"
                  << cfg.codec.keyframe_period << " frames"
                  << ", strips=" << (cfg.codec.strips ? "yes" : "no")
                  << ", H=" << (cfg.codec.homography ? "yes" : "no")
                  << ", mesh=" << (cfg.codec.mesh ? "yes" : "no") << '\n'
                  << "  UDP target: " << cfg.udp.host << ':' << cfg.udp.port << '\n'
                  << "  FlowX wire: v" << static_cast<int>(flowx::kProtocolVersion)
                  << ", max datagram=" << flowx::kMaxUdpDatagramBytes << " B\n";

        if (!cfg.codec.mesh)
            std::cerr << "WARNING: codec.mesh=false is not wired into the product encoder yet; "
                         "mesh remains enabled.\n";

        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);

        flowx::LatestFrame latest;
        std::atomic<bool> source_done{false};
        std::atomic<std::uint64_t> captured_frames{0};

        std::thread capture_thread([&] {
            std::string last_error;
            auto last_error_time = std::chrono::steady_clock::time_point{};

            while (!g_stop.load(std::memory_order_relaxed)) {
                flowx::CapturedFrame frame;
                std::string read_error;
                const flowx::SourceReadResult result = source->read(frame, read_error);

                if (result == flowx::SourceReadResult::Frame) {
                    captured_frames.fetch_add(1, std::memory_order_relaxed);
                    latest.publish(std::move(frame));
                    continue;
                }
                if (result == flowx::SourceReadResult::End) break;

                const auto now = std::chrono::steady_clock::now();
                if (!read_error.empty() &&
                    (read_error != last_error || last_error_time == std::chrono::steady_clock::time_point{} ||
                     now - last_error_time >= std::chrono::seconds(1))) {
                    std::cerr << "source: " << read_error << '\n';
                    last_error = read_error;
                    last_error_time = now;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            source_done.store(true, std::memory_order_relaxed);
            latest.close();
        });

        std::uint64_t latest_sequence = 0;
        std::uint64_t encoded_frames = 0;
        std::uint64_t sent_packets = 0;
        std::uint64_t sent_bytes = 0;
        auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool fatal_error = false;

        while (!g_stop.load(std::memory_order_relaxed)) {
            flowx::CapturedFrame frame;
            if (!latest.waitNext(latest_sequence, frame, g_stop)) {
                if (source_done.load(std::memory_order_relaxed)) break;
                continue;
            }
            if (frame.image.empty()) continue;

            encoder.pushImage(frame.image,
                              cfg.codec.keyframe_bytes,
                              cfg.codec.keyframe_period);
            ++encoded_frames;

            std::vector<flowx::u_char> codec_packet;
            while (encoder.getNextChunk(codec_packet)) {
                std::vector<flowx::u_char> datagram;
                if (!flowx::wrapCodecPacket(codec_packet, stream_id,
                                            frame.capture_timestamp_us,
                                            datagram, &error)) {
                    std::cerr << "FlowX packet wrap failed: " << error << '\n';
                    fatal_error = true;
                    g_stop.store(true, std::memory_order_relaxed);
                    break;
                }
                if (!udp.send(datagram, error)) {
                    std::cerr << "UDP send failed: " << error << '\n';
                    fatal_error = true;
                    g_stop.store(true, std::memory_order_relaxed);
                    break;
                }
                ++sent_packets;
                sent_bytes += datagram.size();
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_report) {
                const std::uint64_t captured = captured_frames.load(std::memory_order_relaxed);
                const std::uint64_t dropped = captured > encoded_frames ? captured - encoded_frames : 0;
                std::cout << "frames captured=" << captured
                          << " encoded=" << encoded_frames
                          << " replaced=" << dropped
                          << " packets=" << sent_packets
                          << " bytes=" << sent_bytes << '\n';
                next_report = now + std::chrono::seconds(2);
            }
        }

        g_stop.store(true, std::memory_order_relaxed);
        latest.close();
        if (capture_thread.joinable()) capture_thread.join();

        const std::uint64_t captured = captured_frames.load(std::memory_order_relaxed);
        std::cout << "FlowX sender stopped: captured=" << captured
                  << " encoded=" << encoded_frames
                  << " packets=" << sent_packets
                  << " bytes=" << sent_bytes << '\n';
        return fatal_error ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "flowx_sender: " << e.what() << '\n';
        return 1;
    }
}

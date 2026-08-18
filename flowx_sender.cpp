#include "flowx_config.h"
#include "flowx_protocol.h"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "Usage: flowx_sender [config.json]\n";
        return 2;
    }

    const std::string config_file = argc > 1 ? argv[1] : "config/flowx_sender.json";
    try {
        const flowx::SenderConfig cfg = flowx::loadSenderConfig(config_file);
        const std::uint32_t stream_id = flowx::generateStreamId();

        std::cout << "FlowX sender\n"
                  << "  config: " << config_file << '\n'
                  << "  stream id: " << stream_id << '\n'
                  << "  source: " << flowx::sourceTypeName(cfg.source.type)
                  << " @ " << cfg.source.fps << " fps\n"
                  << "  codec: " << flowx::keyframeCodecName(cfg.codec.keyframe_codec)
                  << ", key=" << cfg.codec.keyframe_bytes << " B/"
                  << cfg.codec.keyframe_period << " frames"
                  << ", strips=" << (cfg.codec.strips ? "yes" : "no")
                  << ", H=" << (cfg.codec.homography ? "yes" : "no")
                  << ", mesh=" << (cfg.codec.mesh ? "yes" : "no") << '\n'
                  << "  UDP target: " << cfg.udp.host << ':' << cfg.udp.port << '\n'
                  << "  FlowX wire: v" << static_cast<int>(flowx::kProtocolVersion)
                  << ", max datagram=" << flowx::kMaxUdpDatagramBytes << " B\n"
                  << "Sender transport/source implementation follows in the next PR.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "flowx_sender: " << e.what() << '\n';
        return 1;
    }
}

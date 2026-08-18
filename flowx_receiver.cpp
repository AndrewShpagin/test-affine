#include "flowx_config.h"
#include "flowx_protocol.h"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "Usage: flowx_receiver [config.json]\n";
        return 2;
    }

    const std::string config_file = argc > 1 ? argv[1] : "config/flowx_receiver.json";
    try {
        const flowx::ReceiverConfig cfg = flowx::loadReceiverConfig(config_file);

        std::cout << "FlowX receiver\n"
                  << "  config: " << config_file << '\n'
                  << "  UDP listen: " << cfg.udp.bind << ':' << cfg.udp.port << '\n'
                  << "  HTTP listen: " << cfg.http.bind << ':' << cfg.http.port << '\n'
                  << "  HTTP JPEG quality: " << cfg.http.jpeg_quality << '\n'
                  << "  HTTP stream fps: " << cfg.http.stream_fps << '\n'
                  << "  frame: " << cfg.http.frame_endpoint << '\n'
                  << "  stream: " << cfg.http.stream_endpoint << '\n'
                  << "  status: " << cfg.http.status_endpoint << '\n'
                  << "  FlowX wire: v" << static_cast<int>(flowx::kProtocolVersion)
                  << ", max datagram=" << flowx::kMaxUdpDatagramBytes << " B\n"
                  << "Receiver UDP/HTTP implementation follows in later PRs.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "flowx_receiver: " << e.what() << '\n';
        return 1;
    }
}

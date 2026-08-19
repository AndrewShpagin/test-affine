#pragma once

#include "udp_image_codec.h"

namespace flowx {
using u_char = affinecodec::u_char;
using KeyframeCodec = affinecodec::KeyframeCodec;
using PatchData = affinecodec::PatchData;
using LKDebugData = affinecodec::LKDebugData;
using EncoderTiming = affinecodec::EncoderTiming;
using Encoder = affinecodec::Encoder;

// Product decoder wrapper. FlowX v4 omits the historical layered-keyframe end
// datagram, so after every input packet we finalize immediately when all expected
// layers are complete. The underlying AFC1 decoder remains unchanged otherwise.
class Decoder : public affinecodec::Decoder {
public:
    void pushData(const std::vector<u_char>& data) {
        affinecodec::Decoder::pushData(data);
        affinecodec::Decoder::finalizePendingLayeredKeyframe();
    }
};

constexpr std::size_t kMaxCodecPacketBytes = affinecodec::kMaxUdpPacketBytes;
} // namespace flowx

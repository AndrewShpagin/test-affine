#pragma once

#include "udp_image_codec.h"

namespace flowx {
using u_char = affinecodec::u_char;
using KeyframeCodec = affinecodec::KeyframeCodec;
using PatchData = affinecodec::PatchData;
using LKDebugData = affinecodec::LKDebugData;
using EncoderTiming = affinecodec::EncoderTiming;
using Encoder = affinecodec::Encoder;
using Decoder = affinecodec::Decoder;

constexpr std::size_t kMaxCodecPacketBytes = affinecodec::kMaxUdpPacketBytes;
} // namespace flowx

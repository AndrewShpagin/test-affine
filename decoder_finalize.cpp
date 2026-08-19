#include "udp_image_codec.h"

namespace affinecodec {

bool Decoder::finalizePendingLayeredKeyframe() {
    if (!pending_mosaic_.active || pending_mosaic_.layer_count < 2 ||
        pending_mosaic_.layer_count > pending_mosaic_.layers.size())
        return false;

    // Do not publish a partial keyframe merely because one layer arrived first.
    // If UDP loss leaves a layer incomplete, the existing patch-input path can
    // still rebuild from the available complete layers when the first patch for
    // this keyframe arrives.
    for (std::uint8_t i = 0; i < pending_mosaic_.layer_count; ++i)
        if (!pending_mosaic_.layers[i].complete) return false;

    return rebuildMosaicKeyframe();
}

} // namespace affinecodec

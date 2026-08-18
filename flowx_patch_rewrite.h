#pragma once

#include "flowx_codec.h"

#include <string>
#include <vector>

namespace flowx {

// Rewrites AFC1 patch packets after motion estimation so the product sender can
// choose the transmitted residual-mesh density without changing the wire format.
// Non-patch codec packets are left unchanged.
bool reshapePatchMesh(std::vector<u_char>& codec_packet,
                      bool mesh_enabled,
                      int grid_x,
                      int grid_y,
                      std::string* error = nullptr);

} // namespace flowx

# FlowX v4 UDP packet layout

FlowX v4 is the format received directly by `flowx_receiver` over UDP.

There is only one wire magic. AFC1 remains an internal C++ codec representation and is **not transmitted**.

All multibyte values are little-endian. `float` means IEEE-754 `float32`.

## Common header — 20 bytes

Every UDP datagram starts with:

```cpp
#pragma pack(push, 1)
struct FlowXHeaderV4 {
    uint16_t magic;                 // 0x5846, bytes "FX"
    uint8_t  version_type;          // high 4 bits: version=4, low 4 bits: type
    uint8_t  flags;                 // meaning depends on type
    uint32_t stream_id;             // changes when sender process restarts
    uint32_t frame_id;
    uint64_t capture_timestamp_us;  // sender system-clock microseconds
};
#pragma pack(pop)
static_assert(sizeof(FlowXHeaderV4) == 20);
```

Packet types:

```text
1 = KEY_CHUNK
2 = PATCH
3 = LAYER_END
4 = JPEG_RESTART_REGION
```

No `header_size`, `payload_size`, or second magic is transmitted. UDP already supplies the datagram size.
Datagrams are limited to 1300 bytes, including the FlowX header.

---

## Type 1 — KEY_CHUNK

Keyframe JPEG data is split into chunks.

`flags`:

```text
bits 0..1 : layer_index
bits 2..3 : layer_count - 1
bits 4..7 : 0
```

Thus:

```text
layer_count=1 -> classic JPEG
layer_count=2 -> STRIPS
layer_count=3 -> MOSAIC
```

Layout:

```cpp
struct KeyChunkV4 {
    FlowXHeaderV4 h;       // 20
    uint16_t width;        // original image width
    uint16_t height;       // original image height
    uint16_t jpeg_width;   // encoded layer width
    uint16_t jpeg_height;  // encoded layer height
    uint32_t jpeg_bytes;   // total bytes in this JPEG layer
    uint8_t  chunk_index;
    uint8_t  chunk_count;
    uint8_t  data[];       // remainder of UDP datagram
};
```

Fixed part: **34 bytes**.

There is no chunk offset and no chunk payload length. Chunks are concatenated in `chunk_index` order; UDP datagram size gives `data[]` length.

For keyframe packets:

```text
keyframe_id = frame_id
```

### STRIPS

This chunk form is retained for legacy STRIPS and JPEG2000. New JPEG + STRIPS
uses type 4 below.

For two layers:

```text
layer 0 -> output columns 0,2,4,...
layer 1 -> output columns 1,3,5,...
```

---

## Type 2 — PATCH

A patch is one independent UDP datagram referring to a keyframe.

`flags`:

```text
bit 0 : homography perspective terms are present
bit 1 : mesh is present
bits 2..7 : 0
```

Layout:

```cpp
struct PatchV4 {
    FlowXHeaderV4 h;          // 20
    uint16_t keyframe_age;    // keyframe_id = frame_id - keyframe_age
    uint16_t width;
    uint16_t height;
    uint8_t  grid;            // mesh only; low nibble=gx-1, high nibble=gy-1
    float    affine[6];       // 24 bytes

    // if flags & 1:
    float perspective[2];     // 8 bytes

    // if flags & 2:
    // repeated gx*gy times, row-major:
    int16_t mesh_dx;
    int16_t mesh_dy;
};
```

`grid=0` when mesh is disabled.

### Mesh encoding

Mesh displacement is fixed-point `int16`:

```cpp
wire = round(clamp(value_pixels, -255.0f, 255.0f) * 128.0f);
value_pixels = wire / 128.0f;
```

Resolution is **1/128 pixel**. Maximum quantization error before clipping is **1/256 pixel ≈ 0.0039 px**.

Mesh order:

```text
(y=0,x=0), (0,1), ... (0,gx-1),
(y=1,x=0), ...
```

Current product grid range is 2..8 in each dimension.

### Typical PATCH sizes

With homography:

```text
mesh off :  59 bytes
4x4 mesh : 123 bytes
6x6 mesh : 203 bytes
8x8 mesh : 315 bytes
```

For comparison, the previous v3 + AFC1 6x6 homography packet was about 376 bytes.

---

## Type 3 — LAYER_END

Used once after a legacy STRIPS/MOSAIC keyframe so the C++ decoder can finalize
the layered keyframe immediately. JPEG restart regions do not send LAYER_END.

```cpp
struct LayerEndV4 {
    FlowXHeaderV4 h;   // flags encode layer_count as for KEY_CHUNK, layer_index=0
    uint16_t width;
    uint16_t height;
};
```

Size: **24 bytes**.

The WebGL browser decoder does not need this packet; it knows completion from the chunk counts.

---

## Type 4 — JPEG_RESTART_REGION

Used by JPEG + STRIPS. Each packet independently decodes to an 8-pixel-high
region of one half-image. All offsets below include the common 20-byte header.
`version_type = 0x44`, `flags = 0`, and `keyframe_id = frame_id`.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 20 | Common FlowX v4 header |
| 20 | 2 | Original output width |
| 22 | 2 | Original output height |
| 24 | 2 | Encoded half-image width |
| 26 | 2 | Encoded half-image height |
| 28 | 2 | X coordinate in the encoded layout |
| 30 | 2 | Y coordinate in the encoded layout |
| 32 | 2 | Region width in half-image samples |
| 34 | 1 | Fixed JPEG profile: 1 = gray Q85, 2 = YCbCr 4:4:4 Q85 |
| 35 | 1 | Layout: 0 = spatial order, 1 = shared 16x8 tile shuffle v1 |
| 36 | 1–1264 | JPEG entropy data, without restart markers |

The assembled keyframe dimensions are `2 * half_width` by `half_height`,
and are scaled to the original output size when rendered. For layout 0, a decoded sample
`(i,j)` is placed at `(x + 2*i, y + j)`; `x & 1` selects the column half.
Half dimensions and region width are positive multiples of 8.
`floor(x/2)` and `y` are multiples of 8; the full region must fit the half.
Original width must be even; original dimensions cannot be smaller than
the assembled raster. Maximum original area is 16 megapixels.

For layout 1, `(x,y)` describes the shuffled encoded raster. Let
`C = half_width / 8`, `N = C * (half_height / 8)`. Construct the same permutation
for both parities:

1. Initialize `P[i] = i` for `0 <= i < N`.
2. Set the unsigned 32-bit state to `0x46584a31 XOR half_width XOR (half_height << 16)`.
3. For `i = N-1` down to `1`, update the state with xorshift32:
   `s ^= s << 13; s ^= s >> 17; s ^= s << 5`, truncating to 32 bits after each
   operation and using a logical right shift. Swap `P[i]` and `P[s % (i+1)]`.

`P` maps encoded block indices to spatial block indices. For decoded sample
`(u,v)`, compute `b = P[(y/8)*C + floor(x/16) + floor(u/8)]`, then place it at
`((b % C)*16 + 2*(u % 8) + (x & 1), floor(b/C)*8 + v)`.
Update the receipt mask for spatial block `b` and parity `x & 1`. Apply filling
and PATCH transforms only in spatial coordinates. At half size 32x16, the
permutation is `[0,6,5,7,2,1,3,4]`. Frame ID and arrival order do not affect it.
All packets of a keyframe must agree on layout, profile, and dimensions.
Unknown layouts are rejected. Header size and entropy capacity remain unchanged;
the internal AFC1 representation uses byte 31 for the same layout value.

Payload bytes retain JPEG FF00 stuffing and end padding, but contain no raw
markers. The receiver reconstructs an ordinary baseline JPEG using the fixed
profile's standard tables, region width, and height 8. No previous segment,
table packet, or end marker is required. See
[JPEG restart assembly](README_JPEG_RESTART.md) for sizing and mask rules.

Older v4 receivers reject type 4; deploy sender and receiver together.
Receivers with the original type-4 implementation accept layout 0 but reject
layout 1, which previously occupied a reserved-zero byte.

## Loss behavior

A PATCH contains all data needed for that frame relative to its keyframe. Losing one PATCH does not invalidate later PATCH packets.

For type 4, every received region is usable immediately. Missing even/odd data
is copied from the received counterpart. Before presenting a partial key, the
browser fills areas missing from both halves with the nearest usable pixel's
color, retaining the actual receipt masks. Late data for the active key improves
subsequent PATCH rendering without replaying an old frame. Old-key packets are
discarded after a newer key starts.
Legacy type-1 JPEG chunks still require a complete layer before decoding.

For reproducible testing:

```bash
./build/bin/flowx_sender config/flowx_sender_folder.json --loss-percent 10 --loss-seed 1
```

## `/flowx.bin`

`/flowx.bin` is HTTP framing, not the UDP wire protocol. Its `FXB1` records contain the original FlowX v4 UDP datagrams unchanged.
Records can contain individual updates or an accumulated active-key snapshot
followed by the latest compatible PATCH. The latter lets new or slow browsers
catch up without losing received key regions. Parse each contained datagram's
own frame ID; one record may contain both key data and a later PATCH.

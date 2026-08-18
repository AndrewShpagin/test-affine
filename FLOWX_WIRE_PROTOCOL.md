# FlowX UDP wire protocol

This document describes the binary UDP datagrams sent by `flowx_sender` and received by `flowx_receiver`.

The receiver sees a two-level packet format:

```text
UDP datagram
└── FlowX v3 transport envelope (32 bytes, magic "FXV3")
    └── AFC1 codec packet v2 (variable size, magic "AFC1")
```

The outer FlowX header contains session identity and capture time. The inner AFC1 packet contains either a keyframe fragment or a complete motion/mesh patch.

> `/flowx.bin` is **not** the UDP wire format. The HTTP browser transport adds a separate `FXB1` framing record around one or more original FlowX UDP datagrams. The datagrams inside remain unchanged.

## Byte order and primitive types

All integer fields are serialized **little-endian**.

Floating-point values are IEEE-754 32-bit `float`, serialized as their 32-bit bit pattern in little-endian byte order.

The implementation serializes and parses fields explicitly. The packed C/C++ structures below are therefore documentation of the byte layout only; portable code should not `reinterpret_cast` network bytes directly to these structures without handling packing and host endianness.

Current size limits:

- maximum AFC1 codec packet: **1300 bytes**;
- FlowX v3 header: **32 bytes**;
- configured FlowX UDP maximum: **1400 bytes**;
- because the current codec itself is limited to 1300 bytes, current sender datagrams are at most **1332 bytes**.

## 1. FlowX v3 transport envelope

Every UDP datagram starts with this fixed 32-byte header.

| Offset | Size | Type | Field | Meaning |
|---:|---:|---|---|---|
| 0 | 4 | `u32` | `magic` | `0x33565846`; bytes on wire are `F X V 3` |
| 4 | 1 | `u8` | `version` | `3` |
| 5 | 1 | `u8` | `flags` | currently `0` |
| 6 | 2 | `u16` | `header_bytes` | `32` |
| 8 | 4 | `u32` | `stream_id` | non-zero sender/session ID |
| 12 | 4 | `u32` | `frame_id` | source frame ID |
| 16 | 4 | `u32` | `keyframe_id` | reference keyframe ID |
| 20 | 8 | `u64` | `capture_timestamp_us` | source capture time, microseconds since Unix/system-clock epoch |
| 28 | 2 | `u16` | `payload_bytes` | size of the AFC1 packet following this header |
| 30 | 2 | `u16` | `reserved` | must be `0` |
| 32 | N | bytes | `codec_packet` | complete AFC1 codec packet |

Equivalent packed layout for documentation:

```cpp
#pragma pack(push, 1)
struct FlowXHeaderV3 {
    std::uint32_t magic;                // 0x33565846 -> "FXV3"
    std::uint8_t  version;              // 3
    std::uint8_t  flags;                // 0
    std::uint16_t header_bytes;         // 32
    std::uint32_t stream_id;
    std::uint32_t frame_id;
    std::uint32_t keyframe_id;
    std::uint64_t capture_timestamp_us;
    std::uint16_t payload_bytes;
    std::uint16_t reserved;             // 0
};
#pragma pack(pop)

static_assert(sizeof(FlowXHeaderV3) == 32);
```

### Transport semantics

`stream_id` is generated once when a sender process starts. A restarted sender gets a new stream ID. This lets the receiver reset decoder state and reject delayed packets from a previous sender instance.

`frame_id` identifies the source frame. For a keyframe packet, `frame_id == keyframe_id`. For a patch frame, `keyframe_id` identifies the stable reference image against which the patch is defined.

All fragments belonging to one captured frame carry the same `frame_id`, `keyframe_id`, and `capture_timestamp_us`.

The receiver validates that the outer `frame_id` and `keyframe_id` exactly match the same fields inside the AFC1 packet. A mismatch invalidates the datagram.

## 2. AFC1 codec packet v2: common header

The FlowX payload is one complete AFC1 packet. Every AFC1 packet starts with the same 20-byte header.

| Offset | Size | Type | Field | Meaning |
|---:|---:|---|---|---|
| 0 | 4 | `u32` | `magic` | `0x31434641`; bytes on wire are `A F C 1` |
| 4 | 1 | `u8` | `version` | `2` |
| 5 | 1 | `u8` | `type` | packet type, see below |
| 6 | 2 | `u16` | `header_bytes` | total AFC1 header size for this packet type |
| 8 | 4 | `u32` | `frame_id` | frame ID |
| 12 | 4 | `u32` | `keyframe_id` | reference keyframe ID |
| 16 | 2 | `u16` | `original_width` | full output image width |
| 18 | 2 | `u16` | `original_height` | full output image height |

```cpp
#pragma pack(push, 1)
struct Afc1CommonHeaderV2 {
    std::uint32_t magic;          // 0x31434641 -> "AFC1"
    std::uint8_t  version;        // 2
    std::uint8_t  type;
    std::uint16_t header_bytes;
    std::uint32_t frame_id;
    std::uint32_t keyframe_id;
    std::uint16_t original_width;
    std::uint16_t original_height;
};
#pragma pack(pop)

static_assert(sizeof(Afc1CommonHeaderV2) == 20);
```

Current packet types:

```cpp
enum class Afc1PacketType : std::uint8_t {
    KeyframeChunk        = 1,
    Patch                = 2,
    LayeredKeyframeChunk = 3,
    LayeredKeyframeEnd   = 4,
};
```

## 3. Type 1 — classic keyframe chunk

A classic keyframe image can be larger than one UDP packet, so its encoded byte stream is fragmented into several AFC1 packets.

A Type-1 packet has a fixed 40-byte header followed by `chunk_bytes` bytes of encoded image data.

| Offset | Size | Type | Field | Meaning |
|---:|---:|---|---|---|
| 0 | 20 | | `common` | AFC1 common header; `type=1` |
| 20 | 2 | `u16` | `jpeg_width` | width of the encoded keyframe image |
| 22 | 2 | `u16` | `jpeg_height` | height of the encoded keyframe image |
| 24 | 2 | `u16` | `chunk_index` | zero-based fragment index |
| 26 | 2 | `u16` | `chunk_count` | total number of fragments |
| 28 | 4 | `u32` | `jpeg_bytes` | total encoded image size before fragmentation |
| 32 | 4 | `u32` | `chunk_offset` | byte offset of this fragment in the complete encoded image |
| 36 | 2 | `u16` | `chunk_bytes` | bytes following the header |
| 38 | 2 | `u16` | `reserved` | `0` |
| 40 | N | bytes | `data` | encoded image fragment |

```cpp
#pragma pack(push, 1)
struct Afc1KeyframeChunkV2 {
    Afc1CommonHeaderV2 common;  // type = 1, header_bytes = 40
    std::uint16_t jpeg_width;
    std::uint16_t jpeg_height;
    std::uint16_t chunk_index;
    std::uint16_t chunk_count;
    std::uint32_t jpeg_bytes;
    std::uint32_t chunk_offset;
    std::uint16_t chunk_bytes;
    std::uint16_t reserved;
    // std::uint8_t data[chunk_bytes];
};
#pragma pack(pop)

static_assert(sizeof(Afc1KeyframeChunkV2) == 40);
```

For this packet type:

```text
frame_id == keyframe_id
```

The current maximum keyframe-chunk payload is:

```text
1300 - 40 = 1260 bytes
```

The encoded image dimensions can be smaller than `original_width × original_height`. The decoder scales the reconstructed keyframe to/output through the original coordinate system.

The encoded byte stream is currently JPEG in the normal product configuration. The AFC1 packet itself does not carry a separate JPEG/JPEG2000 enum; the native decoder detects the encoded image format from the byte stream.

## 4. Type 3 — layered keyframe chunk

Layered keyframes use the same general fragmentation model but split a keyframe into multiple separately encoded image layers.

Current layer counts:

- `layer_count = 2`: **STRIPS**;
- `layer_count = 3`: **MOSAIC**.

The header is 44 bytes.

| Offset | Size | Type | Field | Meaning |
|---:|---:|---|---|---|
| 0 | 20 | | `common` | AFC1 common header; `type=3` |
| 20 | 1 | `u8` | `layer_index` | zero-based layer index |
| 21 | 1 | `u8` | `layer_count` | 2 for STRIPS, 3 for MOSAIC |
| 22 | 2 | `u16` | `reserved0` | `0` |
| 24 | 2 | `u16` | `jpeg_width` | encoded width of this layer |
| 26 | 2 | `u16` | `jpeg_height` | encoded height of this layer |
| 28 | 2 | `u16` | `chunk_index` | zero-based fragment index within this layer |
| 30 | 2 | `u16` | `chunk_count` | fragment count for this layer |
| 32 | 4 | `u32` | `jpeg_bytes` | total encoded bytes in this layer |
| 36 | 4 | `u32` | `chunk_offset` | byte offset within this layer |
| 40 | 2 | `u16` | `chunk_bytes` | payload bytes following the header |
| 42 | 2 | `u16` | `reserved1` | `0` |
| 44 | N | bytes | `data` | encoded layer fragment |

```cpp
#pragma pack(push, 1)
struct Afc1LayeredKeyframeChunkV2 {
    Afc1CommonHeaderV2 common;  // type = 3, header_bytes = 44
    std::uint8_t  layer_index;
    std::uint8_t  layer_count;
    std::uint16_t reserved0;
    std::uint16_t jpeg_width;
    std::uint16_t jpeg_height;
    std::uint16_t chunk_index;
    std::uint16_t chunk_count;
    std::uint32_t jpeg_bytes;
    std::uint32_t chunk_offset;
    std::uint16_t chunk_bytes;
    std::uint16_t reserved1;
    // std::uint8_t data[chunk_bytes];
};
#pragma pack(pop)

static_assert(sizeof(Afc1LayeredKeyframeChunkV2) == 44);
```

Maximum data payload per layered chunk:

```text
1300 - 44 = 1256 bytes
```

### STRIPS reconstruction

For the current two-layer STRIPS mode, both decoded layer images have the same size `W × H`. The reduced keyframe is reconstructed as `2W × H`:

```text
layer 0 -> x = 0, 2, 4, 6, ...
layer 1 -> x = 1, 3, 5, 7, ...
```

The reconstructed reduced keyframe is then used in the original `original_width × original_height` coordinate system.

MOSAIC uses the same packet structure with `layer_count=3`; only the spatial assembly rule differs.

## 5. Type 4 — layered keyframe end

A layered keyframe is terminated with a small Type-4 packet. It contains no variable payload.

| Offset | Size | Type | Field | Meaning |
|---:|---:|---|---|---|
| 0 | 20 | | `common` | AFC1 common header; `type=4` |
| 20 | 1 | `u8` | `layer_count` | expected layer count |
| 21 | 1 | `u8` | `reserved0` | `0` |
| 22 | 2 | `u16` | `reserved1` | `0` |

```cpp
#pragma pack(push, 1)
struct Afc1LayeredKeyframeEndV2 {
    Afc1CommonHeaderV2 common;  // type = 4, header_bytes = 24
    std::uint8_t  layer_count;
    std::uint8_t  reserved0;
    std::uint16_t reserved1;
};
#pragma pack(pop)

static_assert(sizeof(Afc1LayeredKeyframeEndV2) == 24);
```

For Type 3 and Type 4 packets:

```text
frame_id == keyframe_id
```

## 6. Type 2 — patch packet

A patch frame is not fragmented. The complete motion model for one source frame is stored in one AFC1 packet.

It contains:

1. affine or homography base transform;
2. residual mesh grid;
3. no image payload.

The packet size is variable because the homography adds two floats and the mesh dimensions are variable.

### Fixed patch fields

| Offset | Size | Type | Field | Meaning |
|---:|---:|---|---|---|
| 0 | 20 | | `common` | AFC1 common header; `type=2` |
| 20 | 1 | `u8` | `grid_x` | mesh columns |
| 21 | 1 | `u8` | `grid_y` | mesh rows |
| 22 | 2 | `u16` | `flags` | bit 0 = homography; all other bits currently zero |
| 24 | 24 | `float[6]` | `affine` | affine matrix / homography numerator |
| 48 | 0 or 8 | `float[2]` | `perspective` | present only when homography flag is set |
| ... | `8*grid_x*grid_y` | `float2[]` | `mesh` | `(dx,dy)` residual vectors in row-major order |

There is no payload after the mesh; for Type 2, `header_bytes` is also the complete AFC1 packet size.

```cpp
struct Afc1PatchV2 {
    Afc1CommonHeaderV2 common; // type = 2
    std::uint8_t  grid_x;
    std::uint8_t  grid_y;
    std::uint16_t flags;
    float affine[6];

    // if (flags & 1):
    // float perspective[2];

    // then exactly grid_x * grid_y entries:
    // struct { float dx, dy; } mesh[];
};
```

Patch size formula:

```text
48
+ (homography ? 8 : 0)
+ grid_x * grid_y * 8
```

Typical current sizes, including the outer 32-byte FlowX header:

| Mesh | Homography | AFC1 patch | UDP datagram |
|---|---:|---:|---:|
| disabled (`1×1` zero mesh) | yes | 64 B | 96 B |
| `4×4` | yes | 184 B | 216 B |
| `6×6` | yes | 344 B | 376 B |
| `8×8` | yes | 568 B | 600 B |
| `6×6` | no | 336 B | 368 B |

### Transform representation

Without homography, the six floats represent:

```text
x' = a0*x + a1*y + a2
y' = a3*x + a4*y + a5
```

With `flags & 1`, the same six values are the homography numerator and two extra floats are the perspective denominator terms:

```text
d  = p0*x + p1*y + 1
x' = (a0*x + a1*y + a2) / d
y' = (a3*x + a4*y + a5) / d
```

Coordinates and mesh values are in **pixels**, with image-space coordinates:

```text
(0,0) = top-left
+x     = right
+y     = down
```

The mesh is serialized row-major:

```cpp
index = y * grid_x + x;
```

Each node contains a residual `(dx,dy)` displacement. The C++ decoder expands the grid to a dense output-sized residual field with cubic interpolation. The browser decoder implements the corresponding interpolation in WebGL2.

For inverse rendering, the decoder conceptually performs:

```text
destination pixel
    -> subtract dense residual mesh
    -> apply inverse affine/homography
    -> sample reference keyframe
```

The patch therefore remains relative to `keyframe_id`; patches are not chained frame-to-frame for the transmitted geometric state.

## 7. Parsed semantic data structure

A receiver/parser can expose the wire data with a higher-level structure such as:

```cpp
struct FlowXFrameIdentity {
    std::uint32_t stream_id;
    std::uint32_t frame_id;
    std::uint32_t keyframe_id;
    std::uint64_t capture_timestamp_us;
    std::uint16_t original_width;
    std::uint16_t original_height;
};

struct KeyframeChunk {
    std::uint16_t encoded_width;
    std::uint16_t encoded_height;
    std::uint16_t chunk_index;
    std::uint16_t chunk_count;
    std::uint32_t encoded_bytes;
    std::uint32_t chunk_offset;
    std::vector<std::uint8_t> data;
};

struct LayeredKeyframeChunk : KeyframeChunk {
    std::uint8_t layer_index;
    std::uint8_t layer_count;
};

struct Patch {
    std::uint8_t grid_x;
    std::uint8_t grid_y;
    bool homography;
    std::array<float, 6> affine;
    std::array<float, 2> perspective; // {0,0} when affine-only
    std::vector<std::array<float, 2>> mesh;
};
```

The actual project already exposes equivalent decoded patch information as `PatchData`.

## 8. Frame/keyframe relationship and packet loss

The key property of FlowX for packet-loss tolerance is the reference relationship:

```text
keyframe K
   |-- patch K+1  -> relative to K
   |-- patch K+2  -> relative to K
   |-- patch K+3  -> relative to K
   `-- patch K+4  -> relative to K
next keyframe K+5
```

A lost patch packet loses only that frame; the next patch does not depend on the missing patch's transmitted transform.

A keyframe is larger and therefore fragmented. Missing classic-keyframe fragments prevent that keyframe from being fully reconstructed, so the decoder must continue with its previous valid reference or wait for a later keyframe. Layered keyframes isolate fragments by layer and provide additional opportunities for graceful recovery.

The FlowX v3 outer envelope also lets the receiver distinguish a new sender session from delayed packets belonging to an older sender process.

## 9. Validation checklist for a receiver implementation

Before accepting a UDP datagram, verify:

1. UDP datagram size is within the supported maximum.
2. FlowX magic is `FXV3`.
3. FlowX version is `3`, flags are `0`, header size is `32`, reserved is `0`.
4. `stream_id != 0`.
5. `32 + payload_bytes == UDP datagram size`.
6. AFC1 magic is `AFC1` and AFC1 version is `2`.
7. AFC1 `header_bytes` is valid for the packet type and does not exceed packet size.
8. outer `frame_id/keyframe_id` exactly match inner AFC1 values.
9. keyframe chunks have consistent total size, dimensions, chunk count and offsets.
10. patch packet size exactly matches its homography flag and `grid_x*grid_y` mesh size.
11. unknown flag bits and unsupported packet/layer types are rejected.

This strict validation is important because UDP packets may be lost, duplicated, delayed, or reordered.
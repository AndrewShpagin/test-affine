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
```

No `reserved`, `header_size`, `payload_size`, or second magic is transmitted. UDP already supplies the datagram size.

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

Used once after a STRIPS/MOSAIC keyframe so the existing C++ decoder can finalize the layered keyframe immediately.

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

## Loss behavior

A PATCH contains all data needed for that frame relative to its keyframe. Losing one PATCH does not invalidate later PATCH packets.

A fragmented keyframe is usable only when sufficient keyframe chunks arrive. If a keyframe cannot be reconstructed, the decoder waits for/reacquires a later keyframe; later independent frames are not permanently poisoned.

For reproducible testing:

```bash
./build/bin/flowx_sender config/flowx_sender_folder.json --loss-percent 10 --loss-seed 1
```

## `/flowx.bin`

`/flowx.bin` is HTTP framing, not the UDP wire protocol. Its `FXB1` records contain the original FlowX v4 UDP datagrams unchanged.

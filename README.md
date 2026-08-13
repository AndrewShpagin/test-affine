# test-affine

Experiments for a loss-tolerant image/video codec based on keyframe warping.

Current focus: a UDP-oriented codec prototype with periodic JPEG keyframes and intermediate frames represented by partial-affine motion plus a coarse residual deformation mesh.

## Local test data

Keep flight frames locally in:

```text
local-data/frames/
```

Both `local-data/` and generated `output/` are ignored by Git, so local test images are not accidentally published.

## Build

```bash
cmake -S . -B build
cmake --build build --config Release
```

Executables are written to `build/bin/`.

## VS Code / F5

1. Put images into `local-data/frames/`.
2. Open the repository folder in VS Code.
3. In **Run and Debug**, select `Run codec_test`.
4. Press **F5**.

F5 configures/builds first, then runs from the repository root.

## UDP codec prototype

Files:

```text
udp_image_codec.h
udp_image_codec.cpp
codec_test.cpp
```

Encoder API:

```cpp
affinecodec::Encoder encoder;
encoder.pushImage(image, desired_jpeg_size, keyframe_once_in_N);

std::vector<affinecodec::u_char> data;
while (encoder.getNextChunk(data)) {
    // data is one packet, maximum 1300 bytes
}
```

Decoder API:

```cpp
affinecodec::Decoder decoder;
decoder.pushData(data);

std::vector<affinecodec::u_char> jpeg_data;
if (decoder.updateKeyframe(jpeg_data)) {
    // jpeg_data is a normal JPEG bitstream
}

std::vector<affinecodec::PatchData> patch;
if (decoder.getNextPatch(patch)) {
    // partial-affine + residual mesh in original-image coordinates
}

decoder.render(destination, patch, jpeg_data);
```

Current v1 model:

- Hard packet size limit: `1300` bytes.
- Every input image currently produces one packet.
- Keyframe packet: compact codec header + one ordinary JPEG.
- JPEG is resized to fit the packet. Width and height are multiples of 8.
- Original width/height are transmitted separately, and the decoder expands the JPEG back to that size before warping.
- Motion data therefore does not depend on the transmitted JPEG resolution.
- Intermediate packet: partial-affine transform + `4x4` residual deformation mesh; currently 176 bytes.
- Motion is estimated directly from the current full-resolution keyframe, never chained frame-to-frame.
- LK works on grayscale even when the transmitted JPEG is color.
- Features are selected once per keyframe on an `8x8` grid, up to 3 Shi-Tomasi corners per cell.
- The keyframe LK pyramid is cached and reused for all intermediate frames.
- Decoder caches the decoded/upscaled keyframe and render buffers.
- Missing warped borders use the existing propagation + diffusion fill.
- If LK/partial-affine estimation fails, encoder sends a fresh keyframe instead.

Residual image/refinement tiles are intentionally not part of v1 yet; they can be added later as another packet type.

### End-to-end test

Default:

```bash
build/bin/codec_test
```

Optional arguments:

```bash
build/bin/codec_test <frames> <output> [jpeg-bytes] [key-period]
```

For example:

```bash
build/bin/codec_test local-data/frames output/codec 1300 5
```

The test passes every encoder packet directly into the decoder and writes side-by-side images:

```text
ORIGINAL | DECODED
```

It also prints packet bytes and color MAE for every frame.

## Earlier experiments

`affine_test`, `mesh_test`, and `keyframe_test` remain available for the earlier motion-model experiments.

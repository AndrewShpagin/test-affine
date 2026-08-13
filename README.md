# test-affine

Experiments for a loss-tolerant image/video codec based on keyframe warping.

Current focus: a UDP-oriented codec prototype with periodic JPEG keyframes and intermediate frames represented by partial-affine motion plus a coarse residual deformation mesh.

## Local test data

Keep flight frames locally in:

```text
local-data/frames/
```

Both `local-data/` and generated `output/` are ignored by Git.

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

## UDP codec prototype

```cpp
affinecodec::Encoder encoder;
encoder.pushImage(image, desired_jpeg_size, keyframe_once_in_N);

std::vector<affinecodec::u_char> data;
while (encoder.getNextChunk(data)) {
    // every chunk is <= 1300 bytes
}
```

`desired_jpeg_size` is the target size of the complete keyframe JPEG, not the transport packet size. A 40000-byte JPEG is therefore emitted as roughly 32 chunks.

Decoder:

```cpp
affinecodec::Decoder decoder;
decoder.pushData(data);

std::vector<affinecodec::u_char> jpeg_data;
if (decoder.updateKeyframe(jpeg_data)) {
    // true only after every chunk of the new JPEG has arrived
}

std::vector<affinecodec::PatchData> patch;
if (decoder.getNextPatch(patch)) {
    // partial-affine + residual mesh in original-image coordinates
}

decoder.render(destination, patch, jpeg_data);
```

Current model:

- Hard transport packet limit: `1300` bytes.
- A keyframe JPEG may span many chunks.
- Chunk headers contain frame id, original size, JPEG size, chunk index/count, total JPEG bytes and byte offset.
- Decoder accepts keyframe chunks out of order and ignores duplicates.
- A new keyframe becomes active only when all chunks are assembled.
- If a chunk is lost, the previous valid keyframe remains active; a later keyframe replaces the incomplete assembly.
- JPEG resolution is chosen to approach `desired_jpeg_size`; width and height are multiples of 8.
- Original width/height are transmitted separately and the decoder expands the JPEG before warping.
- Intermediate packet: partial-affine transform + `4x4` residual deformation mesh; currently 176 bytes.
- Motion is estimated directly from the current full-resolution keyframe, never chained frame-to-frame.
- LK works on grayscale even when the transmitted JPEG is color.
- Features are selected once per keyframe on an `8x8` grid, up to 3 Shi-Tomasi corners per cell.
- The keyframe LK pyramid is cached and reused.
- Missing warped borders use propagation + diffusion fill.

Residual image/refinement tiles are intentionally not part of this version yet.

### End-to-end test

```bash
build/bin/codec_test local-data/frames output/codec 40000 5
```

The test feeds every chunk directly into the decoder, writes `ORIGINAL | DECODED`, and prints chunk count, total bytes and color MAE for each frame.

## Earlier experiments

`affine_test`, `mesh_test`, and `keyframe_test` remain available.

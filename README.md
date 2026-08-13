# test-affine

Experiments for a loss-tolerant drone video codec based on image warping.

Current focus: estimate global affine motion with Lucas-Kanade tracking, then model the remaining local motion with a coarse deformation mesh.

## Build

```bash
cmake -S . -B build
cmake --build build --config Release
```

## Affine test

```bash
build/Release/affine_test.exe <frames> <output>
```

The test tracks strong features with pyramidal Lucas-Kanade optical flow, rejects inconsistent tracks with forward/backward checking, and compares partial vs full affine models estimated with RANSAC.

## Mesh test

```bash
build/Release/mesh_test.exe <frames> <output> [grid-x] [grid-y]
```

Default mesh size is `4x4`.

Current mesh model:

1. Track LK features.
2. Estimate robust 6-DOF affine transform with RANSAC.
3. Compute residual motion for each LK track after subtracting affine motion.
4. Estimate residual displacement at coarse mesh nodes with Gaussian weighting.
5. Interpolate the coarse mesh to a dense deformation field.
6. Warp the reference with affine + residual mesh and compare against the next frame.

On the supplied 56-frame 416x416 real drone sequence, the first simple `4x4` mesh reduced mean adjacent-frame grayscale MAE from roughly `8.60` to `7.29` (~15%). Denser meshes did not improve this naive estimator, suggesting that robust local fitting should come before increasing mesh resolution.

# test-affine

Experiments for a loss-tolerant drone video codec based on image warping.

Current focus: estimate global affine motion with Lucas-Kanade tracking, then model the remaining local motion with a coarse deformation mesh.

## Local test data

Keep flight frames locally in:

```text
local-data/frames/
```

Both `local-data/` and generated `output/` are ignored by Git, so the real flight images are not accidentally published.

With no command-line arguments the programs use:

```text
input:  local-data/frames
output: output/affine   or   output/mesh
```

## Build

```bash
cmake -S . -B build
cmake --build build --config Release
```

Executables are written to `build/bin/` for a stable path across CMake generators.

## VS Code / F5

The repository contains `.vscode/tasks.json` and `.vscode/launch.json`.

1. Put images into `local-data/frames/`.
2. Open the repository folder in VS Code.
3. In **Run and Debug**, select either `Run mesh_test` or `Run affine_test`.
4. Press **F5**.

F5 first runs CMake configure/build and then starts the test with the repository root as the working directory, so no command-line paths are needed.

## Affine test

Quick local run:

```bash
build/bin/affine_test.exe
```

Optional overrides:

```bash
build/bin/affine_test.exe <frames> <output>
```

The test tracks strong features with pyramidal Lucas-Kanade optical flow, rejects inconsistent tracks with forward/backward checking, and compares partial vs full affine models estimated with RANSAC.

## Mesh test

Quick local run:

```bash
build/bin/mesh_test.exe
```

Optional overrides:

```bash
build/bin/mesh_test.exe <frames> <output> [grid-x] [grid-y]
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

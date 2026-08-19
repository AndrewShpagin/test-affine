# FlowX sender

`flowx_sender` captures the freshest available image, encodes it with FlowX, compacts the codec output into the FlowX v4 UDP format, and sends the resulting datagrams.

## Sources

- `camera`: OpenCV `VideoCapture`; `device` may be a numeric camera index or a device path such as `/dev/video0`.
- `folder`: sorted image replay at configured FPS, optionally looping.
- `http`: repeated JPEG/image snapshots fetched with cpp-httplib at configured FPS.

Example configs are in `config/flowx_sender.json`, `config/flowx_sender_folder.json`, and `config/flowx_sender_http.json`.

## Grayscale encoding

Set `codec.grayscale=true` to convert every source frame to one-channel grayscale immediately before FlowX encoding:

```json
"codec": {
  "grayscale": true
}
```

This affects the encoded keyframe image and therefore both the C++ and browser outputs become grayscale. Motion estimation already works on grayscale internally, so patch packet structure and patch byte size do not change merely because this option is enabled.

A grayscale JPEG normally needs fewer bytes than a color JPEG at the same dimensions and quality. FlowX keyframes, however, use `keyframe_bytes` as a target and may spend those saved bytes on a larger encoded keyframe. Therefore `grayscale=true` can improve keyframe spatial resolution at the same byte budget instead of always reducing network traffic. To explicitly reduce bandwidth, lower `keyframe_bytes` when grayscale mode is enabled.

## Codec mesh controls

```json
"codec": {
  "mesh": true,
  "mesh_grid_x": 6,
  "mesh_grid_y": 6
}
```

`mesh_grid_x` and `mesh_grid_y` are accepted in the range 2..8. The current motion estimator produces a 6x6 residual field; the product sender resamples it to the configured transmitted grid.

On the v4 wire each mesh `(dx,dy)` uses two signed 16-bit fixed-point values with scale 128. `mesh=false` sets the v4 mesh flag to zero and transmits no mesh bytes.

See `FLOWX_WIRE_PROTOCOL.md` for the binary layout.

## Runtime model

Capture runs on its own thread and publishes into a single latest-frame slot. If encoding is slower than capture, stale frames are replaced rather than queued, so latency does not grow from backlog.

The main thread optionally converts the newest frame to grayscale, encodes it, applies the product mesh setting, compacts each internal codec packet to FlowX v4, and immediately sends the resulting UDP datagram.

## Packet-loss simulation

For repeatable loss-tolerance testing, the sender can deliberately drop complete FlowX UDP datagrams immediately before the socket send:

```bash
./build/bin/flowx_sender config/flowx_sender_folder.json --loss-percent 10 --loss-seed 1
```

Each datagram has an independent 10% drop probability in this example. The seed makes a run reproducible. The sender reports deliberately dropped packets as `sim-loss`; those packets never reach the UDP socket.

`--loss` is accepted as a short alias for `--loss-percent`.

## Build dependencies

Besides OpenCV and nlohmann-json, the sender uses cpp-httplib and threads. On Debian/Raspberry Pi OS the development package is `libcpp-httplib-dev`.

## Run

```bash
./build/bin/flowx_sender config/flowx_sender.json
```

Press Ctrl+C for a clean stop.

# FlowX sender

`flowx_sender` captures the freshest available image, encodes it with FlowX, wraps every codec packet in the FlowX v3 envelope, and sends the resulting datagrams over UDP.

## Sources

- `camera`: OpenCV `VideoCapture`; `device` may be a numeric camera index or a device path such as `/dev/video0`.
- `folder`: sorted image replay at configured FPS, optionally looping.
- `http`: repeated JPEG/image snapshots fetched with cpp-httplib at configured FPS.

Example configs are in `config/flowx_sender.json`, `config/flowx_sender_folder.json`, and `config/flowx_sender_http.json`.

## Codec mesh controls

The sender config exposes the transmitted residual mesh:

```json
"codec": {
  "mesh": true,
  "mesh_grid_x": 6,
  "mesh_grid_y": 6
}
```

`mesh_grid_x` and `mesh_grid_y` are accepted in the range 2..8. The current motion estimator produces the legacy 6x6 residual field; the product sender resamples that field to the configured transmitted grid before the AFC1 patch is wrapped in FlowX v3. This makes the grid a bandwidth/decoder-quality knob without changing the wire format. `mesh=false` transmits a 1x1 zero residual field, which disables mesh deformation in both the C++ and browser decoders.

## Runtime model

Capture runs on its own thread and publishes into a single latest-frame slot. If encoding is slower than capture, stale frames are replaced rather than queued, so latency does not grow from backlog.

The main thread encodes the newest frame and immediately drains all codec packets, applies the configured transmitted mesh shape, adds the FlowX v3 transport header with the sender session ID and capture timestamp, and sends each datagram over UDP.

## Packet-loss simulation

For repeatable loss-tolerance testing, the sender can deliberately drop complete FlowX UDP datagrams immediately before the socket send:

```bash
./build/bin/flowx_sender config/flowx_sender_folder.json --loss-percent 10 --loss-seed 1
```

Each datagram has an independent 10% drop probability in this example. The seed makes a run reproducible. The sender reports the number of deliberately dropped packets as `sim-loss`; those packets never reach the UDP socket. This exercises the real receiver/browser recovery behavior, including fragmented keyframes and independently decodable patch frames.

`--loss` is accepted as a short alias for `--loss-percent`.

## Build dependencies

Besides the existing OpenCV and nlohmann-json dependencies, the sender uses cpp-httplib and threads. On Debian/Raspberry Pi OS the development package is `libcpp-httplib-dev`.

## Run

```bash
./build/bin/flowx_sender config/flowx_sender.json
```

Press Ctrl+C for a clean stop.

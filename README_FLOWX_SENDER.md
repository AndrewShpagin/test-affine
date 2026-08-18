# FlowX sender

`flowx_sender` captures the freshest available image, encodes it with FlowX, wraps every codec packet in the FlowX v3 envelope, and sends the resulting datagrams over UDP.

## Sources

- `camera`: OpenCV `VideoCapture`; `device` may be a numeric camera index or a device path such as `/dev/video0`.
- `folder`: sorted image replay at configured FPS, optionally looping.
- `http`: repeated JPEG/image snapshots fetched with cpp-httplib at configured FPS.

Example configs are in `config/flowx_sender.json`, `config/flowx_sender_folder.json`, and `config/flowx_sender_http.json`.

## Runtime model

Capture runs on its own thread and publishes into a single latest-frame slot. If encoding is slower than capture, stale frames are replaced rather than queued, so latency does not grow from backlog.

The main thread encodes the newest frame and immediately drains all codec packets, adds the FlowX v3 transport header with the sender session ID and capture timestamp, and sends each datagram over UDP.

## Build dependencies

Besides the existing OpenCV and nlohmann-json dependencies, the sender uses cpp-httplib and threads. On Debian/Raspberry Pi OS the development package is `libcpp-httplib-dev`.

## Run

```bash
./build/bin/flowx_sender config/flowx_sender.json
```

Press Ctrl+C for a clean stop.

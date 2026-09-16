# FlowX sender

`flowx_sender` captures the freshest available image, encodes it with FlowX, compacts the codec output into the FlowX v4 UDP format, and sends the resulting datagrams.

## Sources

- `camera`: OpenCV `VideoCapture`; `device` may be a numeric camera index or a device path such as `/dev/video0`.
- `folder`: sorted image replay at configured FPS, optionally looping.
- `http`: repeated JPEG/image snapshots fetched with cpp-httplib at configured FPS.

Example configs are in `config/flowx_sender.json`, `config/flowx_sender_folder.json`, and `config/flowx_sender_http.json`.

For both programs on Windows with a Raspberry Pi HTTP image source, use the
portable [Windows package](README_WINDOWS.md) and `config/flowx_sender_windows.json`.

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

## Runtime codec control (HTTP)

The sender can expose an optional HTTP backend that reads and updates the codec parameters live, in memory. Changes take effect on the next encoded frame and are **not** written back to the config file. All updates are applied under a mutex, so the encode loop always observes a consistent set of parameters.

Enable it with a `control` section in the sender config:

```json
"control": {
  "enabled": true,
  "bind": "0.0.0.0",
  "port": 8090,
  "codec_endpoint": "/codec.json"
}
```

If the `control` section is omitted, the control server stays disabled.

### Endpoints

- `GET <codec_endpoint>` — returns the current codec parameters as JSON.
- `POST <codec_endpoint>` (or `PUT`) — applies a partial JSON patch to the live parameters and returns the merged result.
- `GET /setparam/<name>/<value>[/<name>/<value>...]` — path-based mirror of the JSON patch for quick edits from a browser or `curl`.

Any subset of fields may be sent; omitted fields keep their current value. Supported fields:

| Field | Type | Notes |
| --- | --- | --- |
| `keyframe_bytes` | integer | Target keyframe size in bytes, must be > 0 |
| `keyframe_period` | integer | Keyframe cadence in frames, must be > 0 |
| `keyframe_codec` | string | `"jpeg"` or `"jpeg2000"` (`jpeg2000` requires an OpenCV JP2 writer) |
| `grayscale` | boolean | Convert frames to grayscale before encoding |
| `strips` | boolean | Strip-based keyframes |
| `jpeg_tile_shuffle` | boolean | Shuffle paired 16x8 tiles for JPEG + STRIPS; default false, applied at the next keyframe; decoders restore automatically |
| `homography` | boolean | Homography transform stage |
| `mesh` | boolean | Emit the mesh residual field |
| `mesh_grid_x` | integer | Transmitted mesh grid width, 2..8 |
| `mesh_grid_y` | integer | Transmitted mesh grid height, 2..8 |

Invalid input (wrong type or out-of-range value) is rejected with HTTP `400` and a JSON `{"error": "..."}` body; no partial update is applied.

### Examples

```bash
# Read current parameters
curl http://127.0.0.1:8090/codec.json

# Retune keyframe sizing and cadence in real time
curl -X POST http://127.0.0.1:8090/codec.json \
     -d '{"keyframe_bytes":60000,"keyframe_period":8}'

# Toggle transform stages and mesh grid
curl -X POST http://127.0.0.1:8090/codec.json \
     -d '{"homography":false,"mesh_grid_x":4,"mesh_grid_y":4}'

# Same edits via the GET path API (handy from a browser address bar)
curl "http://127.0.0.1:8090/setparam/keyframe_bytes/5000/grayscale/false"
```

The `/setparam` path takes alternating `name/value` pairs. Booleans accept `true`/`false` or `1`/`0`; integers and `keyframe_codec` use the same names, bounds, and merge behavior as the JSON patch. It returns the merged parameters as JSON, or HTTP `400` on an unknown name, malformed value, or an odd number of segments.

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

## UDP datagram statistics

The periodic and final reports include, for example,
`udp-atomic-limit=1472B udp-over-atomic=2.35%`.
The percentage is the cumulative fraction of **all formed UDP datagrams** whose
payload is strictly larger than the displayed unfragmented-size threshold.
It includes JPEG/JPEG2000 chunks, PATCHes, and end markers, measured before
simulated loss and socket sends. It excludes regions above the absolute UDP
limit that could not be formed into datagrams. With no datagrams it is `0.00%`.

The optional sender setting `udp.mtu` defaults to `1500` (integer, 68–65535).
The threshold subtracts the base IP and UDP headers for the actual connected
address family: 28 bytes for IPv4, 48 for IPv6. Thus MTU 1500 gives 1472/1452
bytes respectively. For example, add `"mtu": 1400` beside `host` and `port` in
the sender's `udp` object to use a 1372-byte threshold with IPv4.
This setting affects statistics only; it does not change packet sizes or sending.
The result estimates fragmentation against your configured path MTU, not observed
IP fragments or automatic path MTU discovery. Account for tunnels or additional
IP headers by lowering the configured MTU as appropriate.

## JPEG datagram statistics

The periodic console report (every two seconds while encoding) and final report
include, for example, `jpeg-chunks=320 jpeg-chunk-avg=1158.4B`.
These are the cumulative JPEG chunk count and mean complete UDP payload size
since sender startup. They include FlowX v4 headers and JPEG data, and exclude
IP/UDP headers, PATCH packets, layer-end markers, and JPEG 2000 chunks.
Both restart-region and classic JPEG chunk packets count. Measurement happens
before simulated loss and socket sends, so deliberately dropped packets and
send failures do not bias the codec size statistic. Before the first JPEG chunk,
the count and average are zero.

The same reports include `jpeg-chunk-max=...B`, `jpeg-chunk-over-target=...`,
and `jpeg-hard-dropped=...`. Maximum and overshoot count are cumulative;
`over-target` counts sizes above 1300 bytes. This is a soft target for restart
JPEG regions: they are sent even above 1400/1472 bytes, with IP fragmentation
allowed on Linux/Windows. Only a region above UDP's absolute 65,507-byte payload
limit is omitted. Its would-be wire size still contributes to these statistics,
and the omission is counted in both `jpeg-hard-dropped` and `failed`.

JPEG + STRIPS compresses each half exactly once, with resolution and restart
interval estimated from the previous keyframe. Measurements adjust the next key,
never re-encode the current one. Classic JPEG/MOSAIC/JPEG2000 retain their existing
encoding paths. Update the receiver and reload `/flowx.html` to accept overshoots.

## Build dependencies

Besides OpenCV and nlohmann-json, the sender uses cpp-httplib and threads. On Debian/Raspberry Pi OS the development package is `libcpp-httplib-dev`.

## Run

```bash
./build/bin/flowx_sender config/flowx_sender.json
```

Press Ctrl+C for a clean stop.

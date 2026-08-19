# FlowX receiver

`flowx_receiver` receives compact FlowX v4 UDP datagrams. The transport adapter reconstructs the internal AFC1 representation for the existing C++ decoder, while the browser path parses v4 directly.

## HTTP endpoints

The default receiver config listens on HTTP port 8080 and exposes:

- `/frame.jpg` — latest C++-decoded frame as JPEG.
- `/stream.mjpg` — C++-decoded MJPEG stream.
- `/status.json` — current FlowX/UDP/decode/frame status.
- `/flowx.html` — built-in direct FlowX browser decoder.
- `/flowx.js` — embedded browser decoder code.
- `/flowx.bin` — original validated FlowX v4 datagrams grouped by source frame.

JPEG encoding for `/frame.jpg` and `/stream.mjpg` is lazy and cached by decoded-frame sequence.

## Direct browser decoder

The browser receives the original v4 datagrams through `/flowx.bin` and does not depend on the C++ image decoder.

- JPEG/STRIPS keyframes use the browser-native JPEG decoder;
- STRIPS interleaving is assembled in WebGL2;
- v4 patch affine/homography fields are parsed directly;
- the fixed-point `int16` mesh is converted with `value = short / 128.0`;
- mesh interpolation, inverse transform, keyframe sampling, border reuse, and final rendering run in WebGL2.

JPEG2000 and three-layer MOSAIC remain intentionally unsupported in the browser path. The normal JPEG + STRIPS sender configuration is the target configuration.

Open:

```text
http://127.0.0.1:8080/flowx.html
```

## C++ decoder compatibility

FlowX v4 does not transmit AFC1 headers. `flowx_protocol_v4.cpp` reconstructs AFC1 packets after UDP parsing so the stable `flowx::Decoder` can remain unchanged. This keeps the native decoder available as a fallback/reference implementation.

See `FLOWX_WIRE_PROTOCOL.md` for the actual UDP layout.

## Session behavior

The receiver follows one active `stream_id`. A new stream is adopted from a keyframe and resets C++ decoder state. Recently retired stream IDs are ignored so delayed UDP packets from an old sender process cannot switch the receiver backwards.

The browser independently resets its keyframe state when `stream_id` changes.

## Local end-to-end test

Terminal 1:

```bash
./build/bin/flowx_receiver config/flowx_receiver.json
```

Terminal 2:

```bash
./build/bin/flowx_sender config/flowx_sender_folder.json
```

Then open:

```text
http://127.0.0.1:8080/flowx.html
http://127.0.0.1:8080/frame.jpg
http://127.0.0.1:8080/stream.mjpg
http://127.0.0.1:8080/status.json
```

Wire round-trip sanity test:

```bash
./build/bin/flowx_protocol_test
```

For a non-HTTP debug check, `--dump-last` remains available:

```bash
./build/bin/flowx_receiver config/flowx_receiver.json --dump-last output/flowx_receiver_last.jpg
```

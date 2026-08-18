# FlowX receiver

`flowx_receiver` receives FlowX v3 UDP datagrams, unwraps the transport envelope, feeds the codec decoder, publishes reconstructed images into a thread-safe `FrameStore`, and exposes both conventional HTTP video output and a raw FlowX browser transport.

## HTTP endpoints

The default receiver config listens on HTTP port 8080 and exposes:

- `/frame.jpg` — latest reconstructed frame as a standalone JPEG.
- `/stream.mjpg` — multipart MJPEG stream suitable for a browser `<img>` element.
- `/status.json` — current FlowX/UDP/decode/frame status.
- `/flowx.html` — built-in direct FlowX browser decoder test page.
- `/flowx.js` — embedded browser decoder code.
- `/flowx.bin` — binary stream of validated FlowX frame bundles.

JPEG encoding for `/frame.jpg` and `/stream.mjpg` is lazy and cached by decoded-frame sequence. Multiple HTTP clients requesting the same frame reuse the same encoded JPEG rather than each encoding another copy. If there are no conventional HTTP image clients, the receiver does not continuously JPEG-encode decoded frames.

The MJPEG output is capped by `http.stream_fps`; if the decoder runs faster, the HTTP stream skips intermediate frames and always uses the freshest available frame.

## Direct browser FlowX transport

`/flowx.bin` does not contain receiver-reencoded JPEG frames. It carries the original validated FlowX v3 datagrams grouped by source frame. A small `FXB1` HTTP framing record contains one complete frame bundle; the FlowX datagrams inside the record are unchanged.

The grouping is intentional. If a browser is slow, the server skips old frame bundles instead of accumulating a TCP backlog. A fragmented keyframe is delivered as one bundle, so client slowness cannot make the browser receive only half of a keyframe. If UDP itself lost part of that keyframe, the browser simply waits for a later complete keyframe.

The first browser milestone decodes **JPEG keyframes only**. It supports classic JPEG keyframes and the current two-layer STRIPS JPEG keyframes. Patch packets are parsed/countable but are not rendered yet; the next browser PR adds the homography/mesh WebGL renderer. JPEG2000 and three-layer MOSAIC are intentionally outside this milestone.

Open:

```text
http://127.0.0.1:8080/flowx.html
```

With the default sender (`keyframe_period = 5`, 20 fps), this page should update at keyframe rate while showing packet/record/patch counters. The following PR will render every patch frame directly in the browser.

## Session behavior

The receiver follows one active FlowX `stream_id`. A different stream is adopted only when a keyframe packet is seen, at which point the codec decoder and pending state are reset. Recently retired stream IDs are ignored so delayed UDP packets from the previous sender process cannot switch the receiver back to an obsolete session.

## Local end-to-end test

Terminal 1:

```bash
./build/bin/flowx_receiver config/flowx_receiver.json
```

Terminal 2:

```bash
./build/bin/flowx_sender config/flowx_sender_folder.json
```

Then open or query:

```text
http://127.0.0.1:8080/flowx.html
http://127.0.0.1:8080/frame.jpg
http://127.0.0.1:8080/stream.mjpg
http://127.0.0.1:8080/status.json
```

For a non-HTTP debug check, `--dump-last` is still available:

```bash
./build/bin/flowx_receiver config/flowx_receiver.json --dump-last output/flowx_receiver_last.jpg
```

The receiver prints UDP and decoded-frame counters every two seconds. Restarting `flowx_sender` while the receiver remains running should produce a new `stream_id`; the receiver should reset the decoder and continue after the new sender keyframe.

The HTTP responses use no-cache headers and allow cross-origin reads for local browser/JS integration.

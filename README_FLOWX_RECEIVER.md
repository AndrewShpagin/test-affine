# FlowX receiver

`flowx_receiver` receives FlowX v3 UDP datagrams, unwraps the transport envelope, feeds the codec decoder, publishes reconstructed images into a thread-safe `FrameStore`, and exposes the latest decoded stream over HTTP.

## HTTP endpoints

The default receiver config listens on HTTP port 8080 and exposes:

- `/frame.jpg` — latest reconstructed frame as a standalone JPEG.
- `/stream.mjpg` — multipart MJPEG stream suitable for a browser `<img>` element.
- `/status.json` — current FlowX/UDP/decode/frame status.

JPEG encoding is lazy and cached by decoded-frame sequence. Multiple HTTP clients requesting the same frame reuse the same encoded JPEG rather than each encoding another copy. If there are no HTTP image clients, the receiver does not continuously JPEG-encode decoded frames.

The MJPEG output is capped by `http.stream_fps`; if the decoder runs faster, the HTTP stream skips intermediate frames and always uses the freshest available frame.

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
http://127.0.0.1:8080/frame.jpg
http://127.0.0.1:8080/stream.mjpg
http://127.0.0.1:8080/status.json
```

For a non-HTTP debug check, `--dump-last` is still available:

```bash
./build/bin/flowx_receiver config/flowx_receiver.json --dump-last output/flowx_receiver_last.jpg
```

The receiver prints UDP and decoded-frame counters every two seconds. Restarting `flowx_sender` while the receiver remains running should produce a new `stream_id`; the receiver should reset the decoder and continue after the new sender keyframe.

## Browser use

A minimal page can display the live stream with:

```html
<img src="http://127.0.0.1:8080/stream.mjpg">
```

The HTTP responses use no-cache headers. The frame and status endpoints also allow cross-origin reads, which is useful when the surrounding UI is served by another local web service.

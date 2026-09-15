# FlowX receiver

`flowx_receiver` receives compact FlowX v4 UDP datagrams. The transport adapter reconstructs the internal AFC1 representation for the existing C++ decoder, while the browser path parses v4 directly.

For a downloadable Windows x64 build, GitHub Actions, and Visual Studio 2022
instructions, see [Windows receiver](README_WINDOWS.md).

## HTTP endpoints

The default receiver config listens on HTTP port 8080 and exposes:

- `/frame.jpg` — latest C++-decoded frame as JPEG.
- `/stream.mjpg` — C++-decoded MJPEG stream.
- `/status.json` — current FlowX/UDP/decode/frame status.
- `/flowx.html` — built-in direct FlowX browser decoder.
- `/flowx.js` — embedded browser decoder code.
- `/flowx.bin` — original validated FlowX v4 datagrams grouped by source frame.

JPEG encoding for `/frame.jpg` and `/stream.mjpg` is lazy and cached by decoded-frame sequence.

For timing tests, `udp.jitter_min_ms`, `udp.jitter_max_ms`, and `udp.jitter_seed`
simulate independent receive delays before both decoding paths. For example,
`0`, `80`, `1` adds 0–80 ms per packet and permits reordering. Defaults `0`, `0`
disable this. See `README_FLOWX_LOSS_TEST.md` for limits, counters, reproducibility,
and combining jitter with sender-side packet loss.

## MJPEG loss concealment

The C++ path now fills lost JPEG restart regions like `/flowx.html`: first the
received even/odd counterpart, then exact Euclidean nearest neighbours, followed
by one adaptive 13-tap smoothing pass. The pass preserves usable pixels and the
one-pixel gap boundary, with a larger radius at the interior/diagonal NN seams.
It uses the restored spatial layout, including shuffled tiles and joined gaps.
This runs on the receiver CPU; no GPU or browser decoder is needed for MJPEG.

Restart keyframes wait for completion, the first matching PATCH, a newer key
burst, or the configured assembly deadline (default 100 ms from the first accepted
packet). Until then the previous published image stays available. Later packets,
including new regions, never extend this deadline. Late regions update
the reference for subsequent PATCH frames without republishing an old keyframe.
NN/filter results are cached until real new samples arrive and always recomputed
from the actual assembly, so blur does not accumulate.

Optional receiver configuration (these are also the defaults when omitted):

```json
"decoder": {
  "fill_gaps": true,
  "smooth_fill": true
}
```

Set `smooth_fill` to `false` for NN-only filling. Set `fill_gaps` to `false` to
disable all restart concealment, including odd/even copies and PATCH border
reuse; absent samples remain black before HTTP JPEG encoding. Debug scaling and
warping use nearest sampling. JPEG re-encoding itself can introduce ringing or
blend narrow black lines, so `/flowx.html?fill_gaps=0` remains the direct view of
missing samples without another JPEG compression step.

These settings apply to `/frame.jpg`, `/stream.mjpg`, and `--dump-last` for all
clients; restart the receiver after editing the config. The browser's checkboxes
and URL options control `/flowx.html` independently. MJPEG retains its existing
latest-frame publication and `http.stream_fps` limit; it does not use the browser's
capture-time playout queue.

## Playback timing settings

Receiver configuration accepts this optional section (values shown are defaults):

```json
"playback": {
  "keyframe_wait_ms": 100,
  "playout_ms": 60
}
```

| Setting | Range | Applies to | Meaning |
| --- | --- | --- | --- |
| `keyframe_wait_ms` | 0–1000 ms | MJPEG and browser | Maximum restart-key assembly wait measured from its first accepted packet. |
| `playout_ms` | 0–200 ms | Browser | Additional buffering of all key/PATCH frames on the capture timeline. |

Both must be integers; 0 disables the corresponding wait. Completion, a matching
PATCH, or a newer key can release assembly before its deadline. With zero assembly
wait, the first valid region can produce a partly filled image immediately.
The timestamp is taken before decoding the first accepted packet; processing
and scheduling can still move actual publication/display beyond the deadline.

These are separate controls: assembly decides when a reference is ready, whereas
playout schedules ready frames. They overlap in time rather than being two fixed
delays added together. Browser display also waits for an animation-frame callback;
MJPEG delivery follows its existing HTTP FPS limit and client buffering.

Restart the receiver to apply config edits. It passes both defaults to `/flowx.js`
and reports them under `playback` in `/status.json`. In `/flowx.html`, **Keyframe
wait** and **Playback buffer** override them for that browser without reconnecting.
Changes are saved in the URL and survive reloads, including filling-mode reloads:

```text
/flowx.html?keyframe_wait_ms=40&playout_ms=60
```

Changing **Keyframe wait** while a key is pending recalculates its deadline from
the original first-packet time. A shorter or zero value can release it immediately;
changing the setting does not replay already published keys. Browser overrides
do not change the shared MJPEG receiver settings.

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

FlowX v4 does not transmit AFC1 headers. `flowx_protocol_v4.cpp` reconstructs AFC1 packets after UDP parsing for `flowx::Decoder`. Native API callers can opt in to the new concealment with `setRestartFillOptions(true, true)` (or `true, false` for NN only). Without this call, the API retains its earlier counterpart-only fill. `setRestartFillOptions(false)` exposes missing samples in black. The receiver enables full concealment by default. Receipt counts are available through `restartReceivedBlocks()` and `restartTotalBlocks()`; estimates never increase them.

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

`ctest --test-dir build --output-on-failure` also checks native/browser fill
equivalence for grayscale, RGB and RGBA. With Node and Python 3 available, the
MJPEG tests launch the real receiver on localhost, inject normal/shuffled UDP
fixtures, and compare `/frame.jpg` and multipart `/stream.mjpg` JPEG bytes to the
native reference in all three fill modes. They cover assembly deadlines, completion,
PATCH/next-key boundaries, late data, duplicates, stream resets, and frame-ID wrap.
Timing tests include non-default and zero waits, later packets near the deadline,
browser defaults/overrides, live UI edits, and rejection of invalid config values.

For a non-HTTP debug check, `--dump-last` remains available:

```bash
./build/bin/flowx_receiver config/flowx_receiver.json --dump-last output/flowx_receiver_last.jpg
```

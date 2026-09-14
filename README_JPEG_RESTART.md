# Independent JPEG regions for STRIPS keyframes

JPEG + STRIPS now sends individually decodable regions. Use the existing
`codec.strips: true` and `codec.keyframe_codec: "jpeg"` settings; no additional
config is needed.
Update sender and receiver together, then reload the browser page.
Classic JPEG, MOSAIC, and JPEG2000 retain their existing chunk formats.

## Encoding and placement

The sender extracts even and odd columns into two half-images, as before.
Each half is optionally downscaled to fit the keyframe byte budget, then encoded
with libjpeg's standard Huffman tables and IJG quality-85 quantization tables.
Profile 1 is grayscale; profile 2 is YCbCr 4:4:4. Both have 8×8 MCUs.
Tables are fixed and regenerated locally by the receiver; no table packet,
full-file header, or end marker must arrive first.

The initial restart interval is calculated from 0.35 bytes/pixel:

```text
entropy budget = 1300 - 36 = 1264 bytes
initial interval <= floor(1264 / (8 * 8 * 0.35)) = 56 MCUs
```

The interval is a divisor of the number of MCU columns. Thus no region crosses
a row, and every decoded region is exactly 8 pixels high. After compression the
sender measures every segment. If any is too large, it reduces the interval
and re-encodes the half until all fit. The 0.35 estimate is only a starting
point, never an assumed compression guarantee. Widths with few divisors can
produce more, smaller packets; this is a deliberate first-version tradeoff.

Each packet carries the bytes between restart markers, with JPEG byte stuffing
and final bit padding preserved. Restart markers are removed. The decoder adds
SOI, fixed DQT/DHT/SOF0/SOS, and EOI locally, with the region width and height 8.
DC prediction starts at zero for every region. There is no dependency on an
earlier segment or on the segment's original RST number.

Coordinates refer to the **encoded keyframe raster**, before it is scaled to
the original output dimensions. Sample `i` is placed at `(x + 2*i, y + row)`.
The low bit of `x` selects even or odd columns. See
[the wire layout](FLOWX_WIRE_PROTOCOL.md#type-4--jpeg_restart_region).

The complete FlowX UDP payload, including its 36-byte header, is at most
1300 bytes. IP and UDP transport headers are additional.

## Loss, late data, and rendering

The first successfully decoded region creates a reference with black holes.
Received 8×8 blocks have separate masks for both half-images. A missing column
sample is filled from its adjacent even/odd counterpart only if that counterpart
was actually received. Filling never marks a block as received.

Later packets overwrite these temporary fills with their actual samples.
Duplicates are ignored. Packets for older keyframes are ignored once a newer
keyframe is active; comparisons support 32-bit frame-ID wraparound.
If neither counterpart arrived, the gap remains black. There is no FEC or
retransmission in this version, and burst losses can remove both counterparts.

Native and browser decoders update the active reference progressively.
The browser keeps the current image visible while a new reference is assembled;
receiving its first region does not display a mostly black keyframe. It presents
a fully received key for playback immediately. The first matching PATCH closes
the key burst and queues both the partial key and PATCH at their respective
capture times. A newer key also closes the prior burst.
If the stream pauses, 100 ms without a newly decoded region presents the partial
key, so missing packets cannot make it wait forever. Duplicates do not extend
this timeout. Canvas resizing is deferred until presentation as well.

After presentation, late regions change only the reference used by subsequent
PATCH frames; they do not replay an old image or clear the previous-frame border
buffer. The native API still signals availability on the first decoded region:
`Decoder::updateKeyframe()` fires once and returns an empty JPEG vector;
`render()` uses the assembled reference directly.

The HTTP raw transport forwards late active-key packets immediately. It retains
the accepted key packets, so a new or slow browser receives a cumulative key
snapshot followed by the latest compatible PATCH when it skips updates.
The snapshot is bounded to 12 MiB and 65534 key packets. Supported original
images are at most 16 megapixels; use an appropriate keyframe byte budget.

## Build and tests

Install libjpeg development headers in addition to the existing dependencies
(`libjpeg-dev` on Debian/Ubuntu, or the corresponding libjpeg-turbo package).
CMake links through `JPEG::JPEG`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The native test covers grayscale and color, difficult MCU-row widths, the
1300-byte limit, 35% loss with shuffled delivery, paired fills, duplicates,
late recovery, stale frames, frame-ID wrap, malformed metadata, subsequent
PATCH rendering, the configured encoder, and HTTP catchup. With Node installed,
CTest also exports native JPEG/RGBA fixtures and checks browser assembly against
the same bytes, including asynchronous decode reordering.
The browser presentation regression test also executes the shipped JavaScript
with a virtual clock and a canvas/WebGL stub, checking that partial keys and
resolution changes preserve the displayed frame until presentation. Playout
tests simulate packet bursts, missing frames, paused tabs, and clock changes.

## Browser frame timing

The browser uses each packet's existing capture timestamp to schedule decoded
images on `requestAnimationFrame`. Rendering is separate from presentation:
up to four GPU snapshots wait for their display times, so successive key/PATCH
renders cannot overwrite an image that is still waiting to be shown. The queue
uses at most five reusable snapshot textures including a staging texture;
each RGBA texture consumes approximately `width * height * 4` bytes.

**Playback delay** in the browser header defaults to **60 ms**, adjustable from
0 to 200 ms. It can also be initialized using `/flowx.html?playout_ms=80`.
This is additional buffering, not a measurement of total end-to-end latency.
If a high frame rate needs more than four queued images, queue pressure shortens
the effective delay and drops older images so playback cannot stall waiting for
frames that have already been evicted.
Use 0 for the next available display refresh, or increase it if network/decode
jitter exceeds the default budget. The default covers one 50 ms frame period at
the sample configurations' 20 fps. Covering a frame period helps a partial key
and its following PATCH occupy separate display slots instead of skipping every
loss-affected keyframe. Sender and browser clocks need not be synchronized;
the first timestamp establishes a relative timeline.

At most one image is presented per display refresh. If playback falls behind,
overdue frames are dropped in favor of the newest due frame; the queue never
grows indefinitely or rapidly replays a backlog. Stream/clock resets discard
queued images. Visible canvas dimensions change only when the new image is
actually presented.

The header distinguishes `renders` from `shown` and reports `playout drops`.
This smooths arrival jitter within the chosen delay budget. It cannot recreate
lost PATCH frames or frames the sender never captured/encoded: those gaps still
hold the most recent available image. No motion interpolation is applied.

For actual JPEG decoding and WebGL rendering, with Playwright and Chromium
installed:

```bash
./build/jpeg_restart_test --export build/restart-fixtures.json
node flowx_restart_webgl_test.js build/restart-fixtures.json
```

An optional third argument supplies the Chromium executable path. This test
checks that late regions change the reference texture while leaving displayed
pixels intact until the next PATCH. It is separate from the Node-only CTest.

These deterministic loss tests validate reconstruction behavior. They do not
replace throughput and visual-quality measurements on the target hardware/link.

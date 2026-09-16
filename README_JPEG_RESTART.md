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

The interval counts consecutive MCUs independently of row width. A segment may
start partway through one row and continue across several rows. The last segment
contains only the remaining blocks; the interval need not divide the image size.
After compression, the sender measures every segment. If any is too large, it
reduces the interval and re-encodes the half until all fit. When segments are
small, it tries up to two larger intervals, aiming for the largest segment to
use 90% of the entropy budget. If a growth attempt exceeds the limit, it keeps
the last fully checked result. This bounds extra growth work and permits smooth
images to exceed the initial 56-MCU estimate. Image size and local complexity
still affect the mean; filling every datagram to 1300 bytes is not guaranteed.

Each packet carries the bytes between restart markers, with JPEG byte stuffing
and final bit padding preserved. Restart markers are removed. The decoder adds
SOI, fixed DQT/DHT/SOF0/SOS, and EOI locally, decoding to a temporary strip whose
width is `8 * block_count` and height is 8, regardless of the half-image width.
DC prediction starts at zero for every region. There is no dependency on an
earlier segment or on the segment's original RST number.

Coordinates refer to the **encoded keyframe raster**, before it is scaled to
the original output dimensions. With `C = half_width / 8`, the starting block
is `(y / 8) * C + floor(x / 16)`. Each subsequent decoded 8x8 block advances
that index, wrapping naturally to the next row. Optional shuffle maps this
encoded index back to its spatial index. The low bit of `x` selects even or odd
columns. No extra coordinates or header bytes are needed. See
[the wire layout](FLOWX_WIRE_PROTOCOL.md#type-4--jpeg_restart_region).

The complete FlowX UDP payload, including its 36-byte header, is at most
1300 bytes. IP and UDP transport headers are additional.

## Optional 16x8 tile shuffle

Set `codec.jpeg_tile_shuffle: true` on the sender to scatter a lost segment
across the image instead of losing consecutive blocks in raster order. The default
is `false`. This setting applies to JPEG + STRIPS; other keyframe formats ignore
it. C++ applications can call `Encoder::setJpegTileShuffle(true)`.

The encoder first selects the encoded resolution and scales both half-images.
It then applies the same deterministic permutation to their 8x8 blocks, which
is equivalent to moving paired 16x8 tiles in the combined encoded raster. Pixels
inside each tile retain their order. Motion estimation keeps the original,
unshuffled image. Quantization and fixed JPEG tables are unchanged; no-loss
decoded pixels match unshuffled encoding at the same dimensions. DC coding and
packet sizes can change, so the existing byte-budget controller may select a
different resolution or restart interval.

Every region carries its layout mode in byte 35: `0` for spatial order, `1` for
shuffle v1. Both the native and browser decoders automatically restore spatial
coordinates from that mode; there is no matching receiver setting to maintain.
No seed packet or coordinate list is needed. The first received region can
initialize the permutation even if all earlier packets were lost. Layout must
remain constant within a keyframe; changes take effect on the next encoded key.

Decoded pixels and receipt masks are placed directly in their original spatial
positions before counterpart filling, nearest-neighbour filling, output scaling,
or PATCH warping. Late packets replace the corresponding real samples. The
browser batches scattered GPU updates until the next render; queued pictures
remain unchanged. The **Fill missing pixels** debug option works with both modes.

When sender HTTP control is enabled, change the option through `/codec.json`
with `{"jpeg_tile_shuffle":true}`, or GET `/setparam/jpeg_tile_shuffle/true`.
HTTP changes are in memory; edit the config file to keep the setting on restart.
Update the receiver and hard-refresh the browser before enabling shuffle:
older versions reject the new layout. Cross-row segments also require updated
receivers, even with shuffle disabled. This disperses losses; it does not recover lost data or guarantee
that missing tiles cannot be adjacent.

## Loss, late data, and rendering

The first successfully decoded region creates a reference with black holes.
Received 8×8 blocks have separate masks for both half-images. A missing column
sample is filled from its adjacent even/odd counterpart only if that counterpart
was actually received. Filling never marks a block as received.

Later packets overwrite these temporary fills with their actual samples.
Duplicates are ignored. Packets for older keyframes are ignored once a newer
keyframe is active; comparisons support 32-bit frame-ID wraparound.
If neither counterpart arrived, the browser fills the gap when the keyframe
wait ends: each missing pixel copies the color of the nearest usable pixel in
the encoded keyframe raster. Existing even/odd counterpart copies take priority.
Distance is Euclidean in pixel coordinates, not block-grid coordinates; ties
prefer the left source, then the upper source. Genuine black source pixels are
valid data. There is no FEC or retransmission in this version.

Native and browser decoders update the active reference progressively.
The browser keeps the current image visible while a new reference is assembled;
receiving its first region does not display a mostly black keyframe. It presents
a fully received key for playback immediately. The first matching PATCH closes
the key burst and queues both the partial key and PATCH at their respective
capture times. A newer key also closes the prior burst.
The configured assembly deadline presents a partial key even if the stream pauses:
by default 100 ms from the first accepted packet, not from the last region.
Neither new regions nor duplicates extend this timeout. Canvas resizing is
deferred until presentation as well.
Nearest-neighbour filling runs before the released key is rendered, whether
release is caused by the assembly timeout, a matching PATCH, or the next key.
The received mask remains unchanged. Late real packets overwrite estimates;
before a subsequent PATCH, remaining holes are refilled from the updated valid
pixels. Already rendered/queued pictures stay unchanged.

The browser then smooths NN-filled interiors with one WebGL2 pass. The radius
and blend strength grow with distance from usable pixels: the first pixel next
to received data is unchanged, and full strength is reached at distance 4.
The radius reaches 3 encoded pixels near the center and diagonal NN seams,
falling toward 1.5 pixels away from those seams. Distances follow the actual
receipt mask, so adjacent lost 16x8 tiles do not introduce artificial filter
borders. Received pixels and even/odd counterpart copies remain unchanged.

The pass uses 13 color taps with bilinear sampling, reads the unmodified NN
texture and writes a separate reference texture. It runs only when the reference
changes, after the existing wait; unchanged PATCH frames reuse the result.
Late real samples trigger a fresh NN fill and smoothing before the next PATCH,
without accumulating blur. A completely recovered reference bypasses smoothing.
This softens NN stars; it cannot reconstruct missing texture or object edges.
Additional storage is 2 CPU bytes and 6 GPU bytes per encoded keyframe pixel,
plus a small block-distance table. No GPU readback is needed in playback.

After presentation, late regions change only the reference used by subsequent
PATCH frames; they do not replay an old image or clear the previous-frame border
buffer. The native API still signals availability on the first decoded region:
`Decoder::updateKeyframe()` fires once and returns an empty JPEG vector;
`render()` uses the assembled reference directly. Nearest-neighbour concealment
described above is enabled in both the browser and the C++ receiver's MJPEG path.
The C++ implementation uses the same quantized radius/blend controls and bilinear
kernel on the CPU, cached between reference updates. Its native API retains the
old counterpart-only fill until `setRestartFillOptions()` is called.
See `README_FLOWX_RECEIVER.md` for the MJPEG fill options and keyframe wait rules.

The HTTP raw transport forwards late active-key packets immediately. It retains
the accepted key packets, so a new or slow browser receives a cumulative key
snapshot followed by the latest compatible PATCH when it skips updates.
The snapshot is bounded to 12 MiB and 65534 key packets. Supported original
images are at most 16 megapixels; use an appropriate keyframe byte budget.

## Debug without filling

**Smooth filled pixels** is enabled by default. Uncheck it, or use
`/flowx.html?smooth_fill=0`, to compare with the previous NN-only fill.
`smooth_fill=1` enables it explicitly. The checkbox reloads the view, preserving
other options and playback delay. This is a browser-only option; sender settings
and JPEG bytes are unaffected. Rebuild/restart the receiver and reload the browser
to load the updated embedded script.

Uncheck **Fill missing pixels** in the browser header, or open
`/flowx.html?fill_gaps=0`. Filling is enabled by default; `fill_gaps=1` enables it
explicitly. Changing the checkbox reloads the view so previously filled pixels
and queued frames cannot survive the switch. Other URL options and the current
playback delay are preserved.

With filling disabled, every unreceived sample stays opaque black, including
missing odd/even columns when only the other half arrived. Areas missing both
halves also stay black after the wait ends. Late packets still replace black
samples with their actual decoded pixels. Receipt masks, completion detection,
and presentation timing are unchanged. Smoothing is also bypassed, and its
checkbox is disabled until filling is enabled again.

Debug rendering uses nearest-pixel key sampling and pixelated canvas scaling
to avoid blending black columns with received neighbours. PATCH areas outside
the keyframe are also black instead of being copied from the previous frame.
The vertical lines are one pixel wide in the encoded keyframe raster; their
display size depends on keyframe/output scaling and browser zoom. This option
controls the browser view only, not native decoding or the wire protocol.

## Build and tests

Install libjpeg development headers in addition to the existing dependencies
(`libjpeg-dev` on Debian/Ubuntu, or the corresponding libjpeg-turbo package).
CMake links through `JPEG::JPEG`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The native test covers grayscale and color, narrow and prime MCU-row widths,
cross-row segments and short tails, adaptive interval growth, exact decoded
pixels against independently encoded 8x8 tiles, the 1300-byte limit,
35% loss with shuffled delivery, paired fills, duplicates,
late recovery, stale frames, frame-ID wrap, malformed metadata, subsequent
PATCH rendering, the configured encoder, and HTTP catchup. With Node installed,
CTest also exports native JPEG/RGBA fixtures and checks browser assembly against
the same bytes, including asynchronous decode reordering and cross-row GPU
upload bounds. Narrow fixtures also exercise browser presentation and live MJPEG
controls. Both layout modes
are tested with shuffled packet delivery and 35% loss, duplicates, late recovery,
and receipt masks. Shuffle checks include a fixed cross-language permutation
vector, no-loss equality with ordinary JPEG, and runtime layout changes.
The browser presentation regression test also executes the shipped JavaScript
with a virtual clock and a canvas/WebGL stub, checking that partial keys and
resolution changes preserve the displayed frame until presentation. Playout
tests simulate packet bursts, missing frames, paused tabs, and clock changes.
Nearest-fill tests compare the result to a brute-force nearest-pixel search,
including image edges, diagonal distances, ties, valid black data, unchanged
receipt masks, and replacement by late packets.
Adaptive-fill tests cover fixed boundaries, merged gaps, image corners, constant
color preservation, NN seam reduction, and recomputation from real samples.
The presentation tests also check the smoothing toggle, one pass per changed
reference, debug bypass, and switching back to raw pixels after full recovery.

## Browser frame timing

The browser uses each packet's existing capture timestamp to schedule decoded
images on `requestAnimationFrame`. Rendering is separate from presentation:
up to four GPU snapshots wait for their display times, so successive key/PATCH
renders cannot overwrite an image that is still waiting to be shown. The queue
uses at most five reusable snapshot textures including a staging texture;
each RGBA texture consumes approximately `width * height * 4` bytes.

**Playback buffer** in the browser header defaults to **60 ms**, adjustable from
0 to 200 ms. It can also be initialized using `/flowx.html?playout_ms=80` or the
receiver's `playback.playout_ms` setting. **Keyframe wait** separately controls
assembly from the first accepted packet (default 100 ms, range 0–1000), configured
with `playback.keyframe_wait_ms` or the browser's `keyframe_wait_ms` URL parameter.
Zero disables the respective wait. Both browser controls apply live and save
their overrides in the URL. See `README_FLOWX_RECEIVER.md` for timing semantics.
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
# Same GPU integration checks with shuffled tiles:
./build/jpeg_restart_test --export-shuffled build/shuffle-fixtures.json
node flowx_restart_webgl_test.js build/shuffle-fixtures.json
```

An optional third argument supplies the Chromium executable path. This test
checks that late regions change the reference texture while leaving displayed
pixels intact until the next PATCH. It is separate from the Node-only CTest.

The actual smoothing GLSL can also be tested without Chromium on Linux with
Node, Python 3, `libEGL.so.1`, and surfaceless OpenGL ES 3 (for example Mesa):

```bash
./build/jpeg_restart_test --export build/restart-fixtures.json
python3 flowx_smooth_fill_egl_test.py build/restart-fixtures.json.js
```

This optional test compiles the shipped shader and compares rendered RGBA bytes
to the CPU kernel reference, including exact preservation wherever blend is zero.
It allows up to two byte levels of bilinear/rounding variation inside filled gaps.

These deterministic loss tests validate reconstruction behavior. They do not
replace throughput and visual-quality measurements on the target hardware/link.

# FlowX packet-loss test

The sender can simulate UDP datagram loss before packets reach the socket. This is intended for end-to-end validation of the codec's loss tolerance with both the C++ and browser decoders.

Start the receiver normally:

```bash
./build/bin/flowx_receiver config/flowx_receiver.json
```

Then run the folder sender with a reproducible loss pattern, for example 10%:

```bash
./build/bin/flowx_sender config/flowx_sender_folder.json --loss-percent 10 --loss-seed 1
```

The loss is applied independently to each complete FlowX v4 UDP datagram after compact wire serialization and immediately before `send()`. The sender reports deliberately dropped datagrams as `sim-loss`.

Useful checks:

- the sender must continue normally while `sim-loss` grows;
- the receiver must not crash or poison later frames after a lost patch packet;
- if a fragmented keyframe is damaged, later patches that depend on it may be unavailable, but decoding must recover at a later usable keyframe;
- `/flowx.html` exercises the direct v4/WebGL decoder;
- `/stream.mjpg` exercises the v4-to-AFC1 C++ decoder path in parallel;
- restarting with the same `--loss-seed` reproduces the same random loss decisions for the same packet sequence.

Suggested loss levels are 1%, 5%, 10%, 20%, and 40%. At high loss rates, visible freezes until a usable keyframe are expected; permanent decoder corruption is not.

## Simulated latency and packet reordering

The receiver can independently delay each UDP datagram before passing it to
either the C++ decoder or `/flowx.bin`. For example, edit its `udp` config:

```json
"udp": {
  "bind": "0.0.0.0",
  "port": 5000,
  "jitter_min_ms": 0,
  "jitter_max_ms": 80,
  "jitter_seed": 1
}
```

Restart the receiver after changing these settings. Each complete packet gets
an independent, uniformly chosen integer delay in the inclusive `[min, max]`
range. Both limits must be integers with `0 <= min <= max <= 1000` ms; the seed
is an unsigned 32-bit integer. Defaults `0, 0` disable the simulator. Equal
nonzero limits add constant latency without introducing random reordering.

Pending packets use a bounded deadline queue. A packet with a shorter delay can
overtake an earlier one, including across frame boundaries. Socket input keeps
running while packets wait, and receiver calls still honor the keyframe timer's
deadline. No per-packet sleeps block the decoder. Capture timestamps and payload
bytes remain unchanged. The assembly wait starts when the first accepted packet
is released from this simulated network into the decoder.

The same seed reproduces assigned delays for the same admitted packet sequence;
wall-clock arrival times and final ordering can still vary with OS scheduling.
The queue is limited to 8192 packets and 8 MiB of payload. A new packet that would
exceed either bound is dropped and counted separately as `overflow`.
These limits prevent a stress test from growing receiver memory indefinitely.

`/status.json` reports `udp.sim_jitter` with `min_ms`, `max_ms`, `seed`, `scheduled`,
`delivered`, `queued`, and `overflow`. Existing `udp.datagrams` counts packets
released to the receiver after simulated jitter; overflow also increases
`udp.ignored`. An idle receiver updates these counters on its receive timeout.

This can be combined with the sender's existing `--loss-percent`/`--loss-seed`
options. It tests `/stream.mjpg` and `/flowx.html` against the same simulated UDP
delivery. The browser's HTTP transport can additionally batch packets; a slow or
new browser receives the existing cumulative key snapshot.

## Frame intervals and buffering

Browser target presentation times are `capture_timestamp + clock_offset +
playout_ms`. Thus steady 20 fps capture targets 50 ms intervals for both KEY and
PATCH frames. Packet arrival gaps do not set these intervals. Actual display is
quantized to animation frames; missing/late frames and queue pressure can cause
holds or drops, so equal wall-clock spacing is not guaranteed.

Key assembly and playout overlap. For a first key received at time 0 and ready
at 30 ms, a 40 ms playout buffer targets presentation at 40 ms, not 70 ms. The
next frame captured 50 ms later targets 90 ms. If a frame is not ready by its
target time, the buffer cannot recover that slot; the browser shows a newer due
frame or waits. `keyframe_wait_ms <= playout_ms` alone is not a guarantee: later
network jitter and decode time can consume the remaining budget. The fixed
four-frame queue can also shorten the effective delay under pressure.

MJPEG retains latest-frame delivery and the `http.stream_fps` cap. It does not
use `playout_ms` or the browser capture-time queue; browser/client MJPEG buffering
can add further timing variation.

CTest includes seeded queue/reordering and overflow checks, plus real receiver
tests for fixed/random delays, unchanged raw browser packets, and MJPEG recovery.

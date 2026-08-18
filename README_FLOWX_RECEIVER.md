# FlowX receiver core

`flowx_receiver` now receives FlowX v3 UDP datagrams, unwraps the transport envelope, feeds the existing codec decoder, and publishes each reconstructed image into a thread-safe `FrameStore`.

HTTP output is deliberately not part of this PR; it is the next layer.

## Session behavior

The receiver follows one active FlowX `stream_id`. A different stream is adopted only when a keyframe packet is seen, at which point the codec decoder and pending state are reset. Recently retired stream IDs are ignored so delayed UDP packets from the previous sender process cannot switch the receiver back to an obsolete session.

## Local end-to-end test

Use the folder sender first because it is deterministic.

Terminal 1:

```bash
./build/bin/flowx_receiver config/flowx_receiver.json --dump-last output/flowx_receiver_last.jpg
```

Terminal 2:

```bash
./build/bin/flowx_sender config/flowx_sender_folder.json
```

Both default configs use UDP port 5000 on localhost. The receiver prints UDP and decoded-frame counters every two seconds.

After several seconds, stop the receiver with Ctrl+C. With `--dump-last`, its most recently reconstructed frame is written to the requested image path so the current sender/UDP/v3/decoder path can be checked visually before the HTTP layer is added.

Useful receiver output looks like:

```text
udp=... bytes=... invalid=0 ignored=0 other-stream=0 resets=0 decoded=... (key=... patch=...) latest=... 416x416
```

Restarting `flowx_sender` while the receiver remains running should produce a new `stream_id`; the receiver should print `FlowX stream changed, decoder reset:` and continue decoding from the new sender keyframe.

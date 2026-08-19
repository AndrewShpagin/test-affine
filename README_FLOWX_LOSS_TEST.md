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

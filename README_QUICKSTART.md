# FlowX quickstart

A first end-to-end test: build the project, start the receiver and sender, and
watch the decoded video in a browser.

## 1. Install dependencies

The project needs a C++17 compiler, CMake, OpenCV, nlohmann-json, and
cpp-httplib. On Debian / Raspberry Pi OS / Ubuntu:

```bash
sudo apt update
sudo apt install -y \
    build-essential cmake \
    libopencv-dev \
    nlohmann-json3-dev \
    libcpp-httplib-dev
```

## 2. Build

```bash
cmake -S . -B build
cmake --build build --config Release
```

Executables are written to `build/bin/`.

## 3. Add test frames

The sample sender configs replay images from a folder. Put a few JPEG/PNG
frames into:

```text
local-data/frames/
```

(A camera or HTTP source can be used instead; see `config/flowx_sender.json`.)

## 4. Run the receiver

In terminal 1:

```bash
./build/bin/flowx_receiver config/flowx_receiver.json
```

It listens for FlowX UDP datagrams and serves HTTP on the `http.port` from the
config (`8002` in this checkout; `8080` is the built-in default).

## 5. Run the sender

In terminal 2:

```bash
./build/bin/flowx_sender config/flowx_sender_folder.json
```

The sender encodes the folder frames and streams them to the receiver over UDP.

## 6. Open it in the browser

Point a browser at the receiver's HTTP port:

```text
http://127.0.0.1:8002/flowx.html
```

`flowx.html` is the built-in browser decoder. Other useful URLs:

```text
http://127.0.0.1:8002/frame.jpg      # latest decoded frame (single JPEG)
http://127.0.0.1:8002/stream.mjpg    # decoded MJPEG stream
http://127.0.0.1:8002/status.json    # UDP / decode / frame status
```

You should see the replayed video reconstructed on the receiver side.

## 7. (Optional) Tune the codec live

If the sender config has `control.enabled = true`, codec parameters can be
changed in memory at runtime from the browser address bar or `curl`:

```text
http://127.0.0.1:8090/codec.json                             # read current params
http://127.0.0.1:8090/setparam/keyframe_bytes/5000/grayscale/false
```

See `README_FLOWX_SENDER.md` for the full parameter list and the JSON `POST`
form.

## Troubleshooting

- **Nothing in the browser:** confirm the sender is running and that both use
  the same UDP `port` (`5000` by default), and that the browser URL uses the
  receiver's configured `http.port`.
- **`address already in use`:** another process holds the UDP or HTTP port;
  stop it or change the port in the config.
- **Blank/older frame only:** the receiver publishes a frame once a full
  keyframe has arrived; give it a moment after the sender starts.

Stop either process with `Ctrl+C`.

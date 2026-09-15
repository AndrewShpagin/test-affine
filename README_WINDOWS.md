# Windows sender and receiver

## Download a GitHub Actions build

1. Open https://github.com/AndrewShpagin/test-affine/actions and select
   **Windows sender and receiver**.
2. Open a successful run for the branch/commit you want (currently
   `JPEG-RESTART-ASSEMBLY`).
3. Under **Artifacts**, download **flowx-windows-x64**. Sign in to GitHub
   if the download is unavailable. Artifacts are retained for 30 days.
4. Extract the entire ZIP into one directory and run **run-receiver.cmd**.

The package contains both x64 Release executables, dependency and MSVC runtime
DLLs, sender/receiver configurations, dependency notices, and build provenance. Keep the
DLLs beside the executable. Visual Studio, vcpkg, Python, and Node are not needed
on the receiving Windows 10/11 x64 PC.

The supplied configuration receives UDP on port **5000** and serves HTTP on
port **8002**:

- Browser decoder: http://127.0.0.1:8002/flowx.html
- MJPEG stream: http://127.0.0.1:8002/stream.mjpg
- Status: http://127.0.0.1:8002/status.json
- Live MJPEG filling/smoothing controls and preview: http://127.0.0.1:8002/mjpg.html

On the receiver, `/decoder.json` supports GET and POST/PUT with boolean
`fill_gaps`/`smooth_fill`. Quick switches include `/setparam/fill_gaps/0` and
`/setparam/smooth_fill/1`. These changes affect all JPEG/MJPEG viewers immediately
when the displayed keyframe is available, and last until restart. The sender
console also reports `jpeg-chunk-avg` in bytes, including FlowX packet headers.

Edit `config/flowx_receiver.json` to change ports, assembly/playout delays, gap
filling, or simulated jitter, then restart the receiver. Point the sender at this
PC's LAN address and allow inbound UDP traffic for the receiver through Windows
Firewall on your private network. Other PCs also need access to its HTTP port to
view the stream. Press Ctrl+C in the console to stop.

## Raspberry Pi image server, both programs on Windows

The Pi supplies HTTP snapshots. The Windows sender downloads and encodes them,
then sends FlowX UDP to the receiver on the same PC at `127.0.0.1:5000`.

1. Edit `config/flowx_sender_windows.json`: set `source.url` to the Pi's full
   snapshot image URL. The example is `http://192.168.2.140:8081/current.jpg`;
   **8081 and /current.jpg are examples, not detected Pi settings**. Use the actual
   port and path. Confirm that opening this URL from Windows returns an image.
2. Keep `udp.host` as `127.0.0.1` and `udp.port` as `5000` for this local test.
3. Start **run-receiver.cmd**, then **run-sender.cmd** in a second window.
4. Open http://127.0.0.1:8002/flowx.html. Receiver status is at
   http://127.0.0.1:8002/status.json and sender codec controls at
   http://127.0.0.1:8090/codec.json.

The HTTP source expects a single JPEG/BMP/PNG image per request, not a webpage,
RTSP URL, or continuous MJPEG stream. For a FlowX receiver on the Pi, use its
`/frame.jpg` snapshot endpoint with its configured HTTP port.

The Windows test config uses 20 FPS, shuffled JPEG strip keyframes, and grayscale,
matching the existing folder test's codec settings. Packet loss starts at 0%.
To simulate 35% packet loss, start the sender from a command prompt with:

```bat
run-sender.cmd --loss-percent 35 --loss-seed 1
```

Receiver jitter settings remain in `config/flowx_receiver.json`. Because the
FlowX UDP path is local in this setup, loss on that path must be simulated; the
Pi-to-PC image download uses HTTP/TCP. Assembly and playout settings still apply.
Close the sender and receiver consoles with Ctrl+C when finished.

## Workflow behavior

`.github/workflows/windows-receiver.yml` runs on pushes to `main` and
`JPEG-RESTART-ASSEMBLY`, and on pull requests. It uses the `windows-2022` hosted
runner with Visual Studio 2022, a pinned vcpkg revision, and cached dependency
builds. The first build takes longer because OpenCV and its dependencies must be
compiled. The job has a 90-minute limit.

The workflow builds both programs plus the existing protocol, JPEG restart,
concealment, browser, and UDP jitter tests. It runs CTest and a UDP-to-HTTP test of
the packaged receiver before uploading the download. It also runs an HTTP image
server and both packaged programs together, checking real key/PATCH decoding and
JPEG output over localhost. The package includes
JPEG, JPEG 2000, and PNG image support; camera capture and optional GUI/DNN modules
are omitted.

`workflow_dispatch` is also enabled. GitHub's **Run workflow** button requires the
workflow file to exist on the default branch (`main`). Until this branch is
merged, pushes to `JPEG-RESTART-ASSEMBLY` start builds automatically; use
**Re-run all jobs** on an existing run to rebuild the same commit.

## Local Visual Studio 2022 build

Install the **Desktop development with C++** workload, CMake tools, and Git.
In the **x64 Native Tools Command Prompt for VS 2022**, from the repository root:

```bat
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
git -C C:\dev\vcpkg checkout 9e44ec0e9f247d77c230ced0ee66c76296837807
call C:\dev\vcpkg\bootstrap-vcpkg.bat
C:\dev\vcpkg\vcpkg.exe install --triplet x64-windows opencv4[core,calib3d,fs,intrinsics,jpeg,openjpeg,png,thread] libjpeg-turbo nlohmann-json cpp-httplib

cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_APPLOCAL_DEPS=ON -DBUILD_TESTING=OFF
cmake --build build-win --config Release --target flowx_sender flowx_receiver --parallel
cmake --install build-win --config Release --prefix dist/flowx-windows-x64
dist\flowx-windows-x64\run-receiver.cmd
```

Use the workflow's artifact for distribution; it also collects dependency
license notices and verifies the packaged executable. Build output alone is at
`build-win/bin/flowx_receiver.exe` and `build-win/bin/flowx_sender.exe`.
MSVC UTF-8 options and the browser string
literal split are already in the source; no manual source edits are needed.

# Windows receiver

## Download a GitHub Actions build

1. Open https://github.com/AndrewShpagin/test-affine/actions and select
   **Windows receiver**.
2. Open a successful run for the branch/commit you want (currently
   `JPEG-RESTART-ASSEMBLY`).
3. Under **Artifacts**, download **flowx-receiver-windows-x64**. Sign in to GitHub
   if the download is unavailable. Artifacts are retained for 30 days.
4. Extract the entire ZIP into one directory and run **run-receiver.cmd**.

The package contains the x64 Release executable, dependency and MSVC runtime
DLLs, receiver configuration, dependency notices, and build provenance. Keep the
DLLs beside the executable. Visual Studio, vcpkg, Python, and Node are not needed
on the receiving Windows 10/11 x64 PC.

The supplied configuration receives UDP on port **5000** and serves HTTP on
port **8002**:

- Browser decoder: http://127.0.0.1:8002/flowx.html
- MJPEG stream: http://127.0.0.1:8002/stream.mjpg
- Status: http://127.0.0.1:8002/status.json

Edit `config/flowx_receiver.json` to change ports, assembly/playout delays, gap
filling, or simulated jitter, then restart the receiver. Point the sender at this
PC's LAN address and allow inbound UDP traffic for the receiver through Windows
Firewall on your private network. Other PCs also need access to its HTTP port to
view the stream. Press Ctrl+C in the console to stop.

## Workflow behavior

`.github/workflows/windows-receiver.yml` runs on pushes to `main` and
`JPEG-RESTART-ASSEMBLY`, and on pull requests. It uses the `windows-2022` hosted
runner with Visual Studio 2022, a pinned vcpkg revision, and cached dependency
builds. The first build takes longer because OpenCV and its dependencies must be
compiled. The job has a 90-minute limit.

The workflow builds the receiver plus the existing protocol, JPEG restart,
concealment, browser, and UDP jitter tests. It runs CTest and a UDP-to-HTTP test of
the packaged executable before uploading the download. The package includes
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
cmake --build build-win --config Release --target flowx_receiver --parallel
cmake --install build-win --config Release --prefix dist/flowx-receiver-windows-x64
dist\flowx-receiver-windows-x64\run-receiver.cmd
```

Use the workflow's artifact for distribution; it also collects dependency
license notices and verifies the packaged executable. Build output alone is at
`build-win/bin/flowx_receiver.exe`. MSVC UTF-8 options and the browser string
literal split are already in the source; no manual source edits are needed.

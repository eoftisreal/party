# Windows 11 build and two-machine test

The client needs only PARTY_FUNCTION_URL. See [capability authentication and migration](CAPABILITIES.md). No accounts, JWT refresh or guest bootstrap remain.

## Build

Install Visual Studio 2022 with Desktop development with C++, CMake and Ninja; Git, Python 3, Node.js 22 and OpenSSL (included in Git for Windows) are needed for the test scripts. Use an x64 Native Tools PowerShell. The manifest pins a vcpkg baseline with FFmpeg 7.1.1; local Linux verification used FFmpeg 6.1.1. Windows CI uses MSVC on Windows Server 2022 and SDL dummy devices; physical Windows 11 output/GPU acceptance remains separate. Consult [the current audit record](AUDIT.md) for actual CI results.

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = 'C:\dev\vcpkg'
git clone https://github.com/eoftisreal/party.git C:\dev\party
cd C:\dev\party
# The user merged the verified implementation into main.
git checkout main
git pull --ff-only
cmake -S . -B build -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
$env:PATH += ";$PWD/build/vcpkg_installed/x64-windows/bin;$PWD/build/vcpkg_installed/x64-windows/tools/ffmpeg"
```

libdatachannel is fetched at a fixed commit, including its pinned submodules. To supply a separately installed libdatachannel, use `-DPARTY_FETCH_DATACHANNEL=OFF` and its CMake package. TLS verification stays enabled.

The default FFmpeg manifest includes x264 and Opus software encoders and the FFmpeg CLI used by test generation. For hardware support, configure with `-DVCPKG_MANIFEST_FEATURES=hardware`; this adds NVENC/QSV/AMF compile support. Drivers and compatible devices are still required. `--software` bypasses GPU probing. The application reports which encoder actually opened. Opening a GPU encoder was **not** verified here. This dependency build includes GPL x264; account for the resulting dependency licenses when distributing binaries.

CI publishes the executable and runtime DLLs in the windows-x64-prototype artifact after a successful build. Preserve the packaged paths or place the vcpkg bin DLLs alongside party.exe, and install the Visual C++ x64 runtime on a PC without Visual Studio. If DLLs are missing, verify the vcpkg `bin` directory on PATH and the CMake app-local dependency copies. Use consistent x64 triplets and Release/Debug libraries. No project API key should be set in the client environment.

## Windows playout scheduling

Each host and viewer process uses its own high-resolution waitable timer for
the 2 ms media pump when Windows supports it. No system-wide timer-resolution
change or busy-wait is required. On older systems, the application falls back
to a normal waitable timer or standard sleep. Run `stats` and inspect
`loop.highResolutionWaitableTimer`, `loop.maxIntervalUs`, and
`loop.lateIntervalsOver15ms` to identify application-loop stalls. These are
start-to-start iteration measurements, **not** a claim of end-to-end display
frame pacing or SDL hardware audio latency. Verify smoothness on a physical
Windows monitor and audio device before drawing conclusions.

## Local media smoke

```powershell
python tools/generate_media.py "$env:TEMP/party-test.mp4"
python tools/run_loopback.py build/party_loopback.exe "$env:TEMP/party-test.mp4"
python tools/run_loopback.py build/party_loopback.exe "$env:TEMP/party-test.mp4" --impair
cd backend
npm ci
cd ..
python tools/two_process.py build/party.exe "$env:TEMP/party-test.mp4" --report two-process.json
```

This opens two real WebRTC peer connections on the same machine and sends actual H.264/Opus over local UDP + DTLS/SRTP. The signaling exchange is in process; it does **not** verify Appwrite or NAT traversal. The test uses SDL dummy audio and decodes video in memory. `--window` also exercises SDL's software drawing with a dummy video driver; it is not a physical display test. `--hardware` requests auto encoder probing, with software fallback. All source media is generated locally and need not be shipped to the viewer. If the bundled FFmpeg CLI lacks lavfi, the generator automatically feeds real moving-checkerboard YUV frames and a PCM tone into its H.264/AAC encoders; --raw selects that path explicitly. No avdevice dependency is needed for the player.

`two_process.py` starts the actual `party.exe host` and `party.exe viewer` as separate processes. A loopback TLS fixture runs the production backend authorization logic with synthetic authentication/in-memory transactions. It tests private invitation joins, early SDP/ICE retries, dropped HTTP signaling/ACK responses, capability authentication, pause/resume, four forward/backward seeks, viewer leave/rejoin, host closure and an empty viewer working directory. Add --outage to exercise 102 seconds of actual HTTP 503 responses, media lease enforcement and fresh-generation recovery. It does **not** prove Appwrite Cloud, live expiry or Internet/NAT behavior. On Windows it also verifies a Unicode host-side movie filename; the executable uses UTF-16 arguments converted to UTF-8 for FFmpeg. The test creates temporary TLS keys and supplies a private CA; TLS verification stays enabled. Never use fixture credentials/server outside tests. With the Visual Studio generator use `build/Release/party.exe` and matching loopback executable paths.

## Appwrite-coordinated streaming

Deploy and smoke-test Appwrite as in [APPWRITE.md](APPWRITE.md). Use the matching v3 backend; no user sessions are created. In **each** user's terminal, clear any diagnostic `PARTY_BIND_ADDRESS` and `PARTY_CA_BUNDLE` from local fixture testing:

```powershell
Remove-Item Env:PARTY_BIND_ADDRESS -ErrorAction SilentlyContinue
Remove-Item Env:PARTY_CA_BUNDLE -ErrorAction SilentlyContinue
$env:PARTY_FUNCTION_URL = 'https://party.sgp.appwrite.run'
```

The `.env.example` is a reference; the binary does not implicitly load a `.env` file. Keep credentials in your existing secret/login mechanism rather than commands committed to Git.

Host PC:

```powershell
.\build\party.exe host --file 'D:\Movies\movie.mp4' --preview --software
# Only after software playback passes, omit --software to probe compatible GPU encoders.
```

Share the printed invitation privately. Up to five viewers can join without accounts:

```powershell
.\build\party.exe viewer --invitation '<room-id>.<invite-code>'
```

Each viewer receives their own encrypted WebRTC stream and needs no local movie. The host encodes one copy of the movie and fans it out across the active peer connections. Close the SDL window or enter `quit` to leave. Host terminal commands: `play`, `pause`, `seek 120`, `stop`, `stats`, `quit`. `seek` uses movie seconds. Controls belong to the host. Preview uses the same stream and playout delay as viewers.

Allow the applications' UDP traffic through Windows Firewall on the networks used for the test. First use two PCs on the same LAN; then different Internet connections. STUN must be reachable for server-reflexive candidates. ICE failures are reported; the host replaces the connection generation after failure, preserving movie position. A disconnected host stops advancing playback until a viewer reconnects, retaining the user's intended play/pause state. A host paused before disconnection stays paused after reconnection. Appwrite outages pause/close media at 100 seconds without a heartbeat; expired membership rejoins, while a replaced session fails closed. No complete movie is uploaded to Appwrite or written by the viewer.

## Direct-only limitation

TURN/relay-only configuration is rejected. Some NAT/firewall pairs cannot connect directly. Surface failures honestly. See [network limitations](TURN.md).

## External acceptance record

Record the selected candidate pair, actual encoder, CPU/RAM, video/audio queue statistics, A/V drift and output-device behavior. Perform: pause/resume, seek forward/backward ten times, late join/rejoin, unplug/replug network, expired membership renewal, room close/expiry, direct ICE failure reporting, and a one-hour movie. Confirm the viewer's working directory remains free of movie files. Calibrate audio/video with flashes and beeps on real speakers/display; the automated test measures scheduling timestamps, not physical lip synchronization. Capture actual failures instead of marking this checklist passed from source inspection.

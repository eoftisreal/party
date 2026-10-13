# Windows 11 build and two-machine test

## Build

Install Visual Studio 2022 with Desktop development with C++, CMake and Ninja; Git and Python 3 are also needed for the test scripts. Use an x64 Native Tools PowerShell. The manifest pins a vcpkg baseline with FFmpeg 7.1.1; local Linux verification used FFmpeg 6.1.1. **Windows builds and device output were not run in the development container.**

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = 'C:\dev\vcpkg'
git clone https://github.com/eoftisreal/party.git C:\dev\party
cd C:\dev\party
# Check out the development PR branch until it is merged.
git checkout feat/webrtc-streaming-prototype
cmake -S . -B build -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
$env:PATH += ";$PWD/build/vcpkg_installed/x64-windows/bin;$PWD/build/vcpkg_installed/x64-windows/tools/ffmpeg"
```

libdatachannel is fetched at a fixed commit, including its pinned submodules. To supply a separately installed libdatachannel, use `-DPARTY_FETCH_DATACHANNEL=OFF` and its CMake package. TLS verification stays enabled.

The default FFmpeg manifest includes x264 and Opus software encoders and the FFmpeg CLI used by test generation. For hardware support, configure with `-DVCPKG_MANIFEST_FEATURES=hardware`; this adds NVENC/QSV/AMF compile support. Drivers and compatible devices are still required. `--software` bypasses GPU probing. The application reports which encoder actually opened. Opening a GPU encoder was **not** verified here. This dependency build includes GPL x264; account for the resulting dependency licenses when distributing binaries.

If DLLs are missing, verify the vcpkg `bin` directory on PATH and the CMake app-local dependency copies. Use consistent x64 triplets and Release/Debug libraries. No project API key should be set in the client environment.

## Local media smoke

```powershell
python tools/generate_media.py "$env:TEMP/party-test.mp4"
python tools/run_loopback.py build/party_loopback.exe "$env:TEMP/party-test.mp4"
python tools/run_loopback.py build/party_loopback.exe "$env:TEMP/party-test.mp4" --impair
```

This opens two real WebRTC peer connections on the same machine and sends actual H.264/Opus over local UDP + DTLS/SRTP. The signaling exchange is in process; it does **not** verify Appwrite or NAT traversal. The test uses SDL dummy audio and decodes video in memory. `--window` also exercises SDL's software drawing with a dummy video driver; it is not a physical display test. `--hardware` requests auto encoder probing, with software fallback. All source media is generated locally and need not be shipped to the viewer.

## Appwrite-coordinated streaming

Deploy and smoke-test Appwrite as in [APPWRITE.md](APPWRITE.md). Create two user sessions through your existing login integration. In **each** user's terminal:

```powershell
$env:PARTY_FUNCTION_URL = 'https://your-function-domain.appwrite.run'
$env:PARTY_APPWRITE_ENDPOINT = 'https://your-region.cloud.appwrite.io/v1'
$env:PARTY_PROJECT_ID = 'your-project'
$env:PARTY_DATABASE_ID = 'party-v2'
$env:PARTY_SESSION_SECRET = '<this-user-session-secret>'
# Or use PARTY_JWT_FILE and refresh it through your login integration.
$env:PARTY_ICE_SERVERS = '[{"url":"stun:your-stun-server:3478"}]'
```

The `.env.example` is a reference; the binary does not implicitly load a `.env` file. Keep credentials in your existing secret/login mechanism rather than commands committed to Git.

Host PC:

```powershell
.\build\party.exe host --file 'D:\Movies\movie.mp4' --preview
# Use --software for a reproducible software-only test.
```

Share the printed room ID and invite code privately. On the viewer PC, use a **different user's** session:

```powershell
.\build\party.exe viewer --room '<room-id>' --invite '<invite-code>'
```

The viewer receives the live encoded stream and needs no local movie. Close the SDL window or enter `quit` to leave. Host terminal commands: `play`, `pause`, `seek 120`, `stop`, `stats`, `quit`. `seek` uses movie seconds. Controls belong to the host. Preview uses the same stream and playout delay as viewers.

Allow the applications' UDP traffic through Windows Firewall on the networks used for the test. First use two PCs on the same LAN; then different Internet connections. STUN must be reachable for server-reflexive candidates. ICE failures are reported; the host replaces the connection generation after failure, preserving movie position. A disconnected host stops advancing playback until a viewer reconnects. No complete movie is uploaded to Appwrite or written by the viewer.

## Optional UDP TURN

Supply a server you operate or have permission to use, with credentials provisioned at runtime:

```powershell
$env:PARTY_ICE_SERVERS = '[{"url":"stun:stun.example:3478"},{"url":"turn:turn.example:3478","username":"short-lived-user","credential":"short-lived-secret"}]'
$env:PARTY_RELAY_ONLY = '1'
```

Forced relay is useful for verification. TURN forwards encrypted packets and consumes relay bandwidth; there is no bundled free TURN service or embedded credential. The pinned libjuice build rejects TCP/TLS TURN. Repeat the test with different NATs and firewall restrictions, including a cellular network. These infrastructure tests have not run here.

## External acceptance record

Record the selected candidate pair, actual encoder, CPU/RAM, video/audio queue statistics, A/V drift and output-device behavior. Perform: pause/resume, seek forward/backward ten times, late join/rejoin, unplug/replug network, JWT refresh beyond 15 minutes, room close/expiry, failed TURN credentials, relay-only mode, and a one-hour movie. Confirm the viewer's working directory remains free of movie files. Calibrate audio/video with flashes and beeps on real speakers/display; the automated test measures scheduling timestamps, not physical lip synchronization. Capture actual failures instead of marking this checklist passed from source inspection.

# Party — C++20 live watch-party prototype

A host streams a local movie to one remote viewer using **H.264/Opus over encrypted WebRTC RTP**. The viewer needs no copy of the movie. Appwrite coordinates authenticated rooms, host authorization, signaling and membership; it does not carry media.

The standalone `party` application supports host play/pause/seek/stop, optional local preview, viewer playback, ICE connection replacement, bounded jitter buffering, NACK/PLI recovery, network statistics and configurable UDP STUN/TURN. The transport is pinned libdatachannel; media decoding/encoding is FFmpeg; prototype output is SDL2. Viewer media remains in memory.

**Status:** real 1080p local peer streaming has been tested, including controls, reconnection and separate host/viewer processes against a local TLS signaling fixture. Appwrite's live deployment, Realtime, cross-Internet NAT/TURN, physical Windows output and GPU encoding still require external verification. The congestion controller is a prototype, not production libwebrtc/GCC. See the measured results and limitations before integration.

- [Verified Windows/Linux results and downloadable executable](docs/verification/completion.md)
- [Windows build and remote test](docs/WINDOWS.md)
- [Appwrite setup, schema and API](docs/APPWRITE.md)
- [Architecture and transport evaluation](docs/ARCHITECTURE.md)
- [Post-merge audit and current verification](docs/AUDIT.md)
- [Original tests and measurements](docs/TESTS.md)
- [Integration into the existing player](docs/INTEGRATION.md)

## Reproducible build presets (CMake 3.25+)

The repository includes `CMakePresets.json` for separate Linux Release and
ASan/UBSan build directories, plus an MSVC x64 Release preset. Linux retains
system package dependencies; Windows uses the existing pinned `vcpkg.json`
manifest. These are convenience presets, not a replacement for the native
media dependencies or the two-PC acceptance tests.

```sh
# Linux (after installing the packages below)
cmake --preset linux-asan-ubsan
cmake --build --preset linux-asan-ubsan --parallel 2
ctest --preset linux-asan-ubsan
# For an unsanitized build: replace linux-asan-ubsan with linux-release.
```

On Windows, set `VCPKG_ROOT` to the directory containing
`scripts/buildsystems/vcpkg.cmake` and install Visual Studio 2022/C++ before
running `cmake --preset windows-msvc-release`,
`cmake --build --preset windows-msvc-release`, and
`ctest --preset windows-msvc-release`. The Linux sanitizer preset is tested in
CI; the Windows preset does not claim Windows AddressSanitizer coverage.

## Linux quick test

```sh
sudo apt-get install cmake ninja-build pkg-config libavcodec-dev libavformat-dev \
  libavutil-dev libswscale-dev libswresample-dev libsdl2-dev \
  libcurl4-openssl-dev nlohmann-json3-dev libssl-dev ffmpeg
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
python3 tools/generate_media.py /tmp/party-test.mp4
python3 tools/run_loopback.py build/party_loopback /tmp/party-test.mp4
python3 tools/run_loopback.py build/party_loopback /tmp/party-test.mp4 --impair
cd backend
npm ci
npm test
npm run check
```

The generated movie is a real encoded test-pattern video with audio. The loopback test uses two real peer connections over UDP/DTLS/SRTP and software decoding; its signaling is in process, so it does not prove Appwrite or NAT traversal. `tools/run_loopback.py` also measures combined process resources and checks for working-directory writes.

## Remote use

Deploy Appwrite and supply each user's own session/JWT, endpoint/project and optional ICE server configuration as documented. The executable does not load `.env` files automatically.

```sh
# Host: creates a private room and prints room ID + invite.
./build/party host --file /path/to/movie.mp4 --preview
# Viewer: different user's credentials, no movie file required.
./build/party viewer --room ROOM_ID --invite INVITE_CODE
```

Windows equivalents and DLL/dependency setup are in the Windows guide. Host commands: `play`, `pause`, `seek 120`, `stop`, `stats`, `quit`. `--software` selects libx264; `--headless` is useful for testing with a configured dummy audio device. Default scope is one host + one viewer, SDR up to 1080p, stereo audio. No download endpoints or original-file transfers are implemented.

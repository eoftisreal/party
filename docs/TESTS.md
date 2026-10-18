> Historical record for earlier commits, not verification of the v3 capability protocol. See CAPABILITIES.md and current PR checks.

# Verification record

The original PR #1 results below are historical. See [the post-merge audit](AUDIT.md) for new regression tests, separate-process measurements, defects discovered during verification, and current CI/external requirements.

Local verification completed on 2026-10-13 in the Ubuntu 24.04 development container, GCC 13, FFmpeg 6.1.1, libdatachannel 0.24.1 at the pinned commit. The test movie is generated `testsrc2` at **1920×1080/30fps**, with real encoded audio. No original movie is included in Git.

## Ran and passed

| Test | Evidence |
| --- | --- |
| Backend unit/integration boundary tests | 14 tests passed; JWT rejection, spoofed identity, invite rejection, host-only offers, negotiation order, stale sessions/generations, leases/expiry/close, acknowledgements, strict SDP/ICE validation, rate limits and concurrent capacity |
| Actual node-appwrite SDK request-shape test | Verified transactionId/commit/rollback and private signal/recipient-only notification permissions by intercepting SDK HTTP calls; not a live server test |
| C++ core test executable | NTP estimator, malformed clock samples, RTP time wrap, bitrate limits, secure IDs and malformed RTP headers |
| C++ RTP test executable | Sequence wrap with FU-A reordering, NACK, missing-fragment discard, recovery, invalid STAP-A and truncated RTCP rejection |
| Real clean peer stream | Real ICE connectivity checks, DTLS/SRTP, H.264/Opus transmission, decoded luma checksum and nonzero audio energy |
| Controls | Pause freezes rendering; resume, forward seek, stop and replacement peer connection complete in the actual media test |
| Reconnection/late-generation recovery | Both PCs are closed/recreated mid-test; movie resumes from its current position and viewer decodes/renders again |
| Loss/reordering | A sender-side handler drops approximately every 97th video RTP packet and reorders approximately every 31st, after the NACK cache and before encryption/pacing. Real encrypted retransmissions recover missing packets; final receiver generation reports NACKs and zero unrecovered loss |
| Viewer persistence check | Temporary combined host/viewer working directory stayed empty in every passing run; source review confirms no viewer muxer/movie writer/download endpoint |
| SDL output code | SDL software renderer exercised with a dummy video driver; dummy audio output used. Not a physical monitor/speaker test |
| Unavailable GPU fallback | Hardware probing requested; unusable/absent GPU encoders fell back to libx264. No GPU encoded frames were verified |
| ASan/UBSan | Both unit executables plus clean/impaired 1080p streaming passed with `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1` |
| Backend syntax and Git whitespace | Passed |

LeakSanitizer initially failed because it could not inspect `/proc/<pid>/task` in this execution environment. Leak detection was disabled to run ASan/UBSan. This is **not a leak-check pass**. Dependency libraries were not rebuilt with sanitizer instrumentation; sanitizer coverage applies to the application/core code. No TSAN result is claimed.

## Measured passing runs

Each run lasts about 17 seconds and includes pause/resume, seek, a new connection generation, and stop. Frame counts exclude different amounts of media intentionally dropped at control transitions; they are not a 30fps steady-state throughput benchmark. Resource values cover **both host and viewer in one process**, with source decoding, encoding and receive decoding.

| Run | Decoded video | Rendered video | Decoded audio packets | Max A/V scheduling drift | CPU time | Peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Release, dummy SDL renderer, hardware probe → libx264 | 361 | 319 | 619 | 33.30ms | 10.619s | 195.5MiB |
| Release, controlled RTP loss/reordering | 361 | 317 | 617 | 33.34ms | 9.830s | 169.3MiB |
| ASan/UBSan, clean stream | 362 | 320 | 619 | 33.33ms | 9.990s | 571.6MiB |
| ASan/UBSan, controlled RTP loss/reordering | 361 | 319 | 619 | 33.35ms | 9.913s | 570.8MiB |

All four recorded runs reported zero decoder errors, zero sender pacing drops, zero rebufferings and empty working directories. `buffering=true`/zero audio queue at the final snapshot reflects the test's explicit stop. Network counters in the final report describe the **second connection generation**, while player/source counters span both generations. SCTP RTT is not a measurement of video end-to-end latency. A/V drift measures video scheduling relative to the actual queued-audio clock, not physical lip-sync calibration.

Full machine-readable reports: [release renderer/fallback](verification/release-window.json), [release impaired](verification/release-impaired.json), [sanitized clean](verification/asan-loopback.json), [sanitized impaired](verification/asan-impaired.json).

## Reproduce

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
python3 tools/generate_media.py /tmp/party-test.mp4
python3 tools/run_loopback.py build/party_loopback /tmp/party-test.mp4
python3 tools/run_loopback.py build/party_loopback /tmp/party-test.mp4 --impair
python3 tools/run_loopback.py build/party_loopback /tmp/party-test.mp4 --window --hardware
```

The local test binds explicitly to `127.0.0.1` to avoid interface enumeration blocked by the development container. The production application performs normal candidate gathering. No injected media frames are used: the generated movie is read, decoded, encoded, transmitted, received, decoded and scheduled. Controlled packet loss is an integration injection, **not an external router/netem test**. SRTP replay warnings from duplicate retransmission requests can appear during impaired runs; the reported recovered loss and decoder counters are the relevant outcomes.

For sanitizers, reconfigure with `-DPARTY_SANITIZE=ON`. Run leak detection normally on an unrestricted host; only this container required disabling it. CI jobs are provided for backend, Linux sanitizers and Windows software streaming. Adding a workflow does not establish that it has passed; consult the PR checks.

## Not run / remaining acceptance work

- Live Appwrite schema setup, transaction conflict behavior on a deployed server, Realtime subscription, JWT/session refresh beyond 15 minutes, live authorization/expiry smoke. Local fixtures do not prove cloud behavior.
- Separate physical machines, Internet/NAT traversal, public STUN, UDP TURN and relay-only mode, actual cable/network outage and automatic Appwrite-coordinated reconnection.
- Physical Windows 11 rendering/audio, WASAPI/device latency, real GPU NVENC/QSV/AMF encoding, HDR or 4K. Windows CI compilation is tracked separately from hardware acceptance.
- Real display/speaker flash-and-beep calibration, one-hour drift, sustained congestion/competing flows, adaptive resolution, different codecs/containers and hostile input-file fuzzing.
- Separate-process/machine filesystem tracing of the viewer. The application has no viewer movie writer, but the automated empty-directory check is scoped to the local combined test.
- Main-player integration and multiviewer support.

Use [the Windows external acceptance steps](WINDOWS.md) and the live Appwrite smoke script to close these gaps. The prototype has genuine local streaming evidence; it is not presented as a fully validated production Internet watch-party service.

> Historical record for earlier commits, not verification of the v3 capability protocol. See ../CAPABILITIES.md and current PR checks.

# Final verification — 2026-10-14

The user merged PR #3 into `main` as `4c8d1e90a4aca75c30ebd5cae17ce092473146bc` while verification was finishing. A fresh fetch confirmed that its complete Git tree (`c1082859ee34ab70191a204cf60148c1a4d42cd1`) exactly matches tested PR head `90c64c5f0e791d24fa324227991113c9bd806244`. This report adds documentation only; no further runtime changes are required to obtain that tested implementation. The agent did not merge any PR.

## Executed checks

[GitHub Actions run 38513823354](https://github.com/eoftisreal/party/actions/runs/38513823354) completed successfully. Results were checked from job steps and actual logs, not inferred from the workflow definition.

| Check | Result and scope |
| --- | --- |
| Backend | 24 tests passed in CI and independently rerun locally; syntax checks passed. Includes authorization, sessions, queue/deduplication, SDK request shapes, schema validation, and stricter SDP checks. |
| Linux | C++20 build and all three CTest executables passed with ASan/UBSan enabled. Clean, impaired, and raw-input-fallback real media loopbacks passed. |
| Windows | MSVC x64 build and all three CTest executables passed on Windows Server 2022. Clean and impaired real media loopbacks passed. |
| Separate executables, both OSes | Actual `party host` and `party viewer` passed private invitation, H.264/Opus playback, pause/resume, repeated forward/backward seeks, viewer exit/rejoin, host closure, lost signaling/ACK responses and synthetic JWT 401 renewal tests. Windows also exercised a Unicode movie path. |
| Coordination outage, both OSes | 102 seconds of HTTP failures followed by a fresh generation and successful recovery; paused intent preserved. |
| Viewer storage | Viewer working directories remained empty. Source audit found no viewer movie writer or complete-file download endpoint. This is not system-wide filesystem tracing. |

Native signaling tests cover post-application ACKs, retry/deduplication, generation/session replacement, and independent outbound backoff for 429, 503, transaction conflicts, and transport failures. The RTP unit test specifically covers a sequence gap beyond 1,024 packets, partial-frame discard, and delayed pre-gap packets. Actual media impairment tests inject smaller packet loss/reordering at the RTP sender handler; the long-burst regression is a receiver unit test, not a WAN loss experiment.

The fresh local native rebuild attempted after resuming this chat was blocked by a missing extracted FFmpeg linker symlink. It is not counted as a local native pass. The successful Linux and Windows CI builds/tests above independently verify the exact source tree now on main. No GPU device was available locally.

## Measured separate-process results

These are short control/recovery tests with dummy SDL output and same-machine UDP/ICE/DTLS/SRTP. Coordination uses the production backend routes with a local TLS fixture, synthetic authentication and in-memory transactions; it is not Appwrite Cloud.

| Measurement | Linux, sanitizer build | Windows, Release |
| --- | ---: | ---: |
| Total test wall time | 175.042 s | 180.781 s |
| First viewer rendered frames | 915 | 902 |
| Replacement viewer rendered frames | 127 | 123 |
| Maximum A/V scheduling drift across viewers | 34.330 ms | 54.334 ms |
| Decoder errors | 0 | 0 |
| Recovery elapsed from outage start | 110.033 s | 115.031 s |
| Viewer working-directory files | 0 | 0 |

Linux measured 31.746 user CPU seconds plus 2.870 system CPU seconds for all child processes (host, viewers, Node fixture/smoke and OpenSSL). Largest-child peak RSS was 681,068 KiB with sanitizers enabled; this is neither concurrent aggregate memory nor a Release memory benchmark. Windows CPU/RAM were not measured by this runner. Scheduling drift is not physical display/speaker lip-sync calibration.

Raw process reports extracted without numeric re-serialization from the successful job logs: [Linux](completion-linux-process.json), [Windows](completion-windows-process.json). Both report `passed: true`, empty viewer directories, fresh-generation recovery and the deliberately dropped responses. They contain no account credentials, invitation codes or movie content.

## Download and reproduce

The successful run publishes `windows-x64-prototype`, containing `party.exe` and runtime DLLs:

- [Windows executable artifact](https://github.com/eoftisreal/party/actions/runs/38513823354/artifacts/11871599055)
- [Linux process-report artifact](https://github.com/eoftisreal/party/actions/runs/38513823354/artifacts/11873060160)
- [Windows process-report artifact](https://github.com/eoftisreal/party/actions/runs/38513823354/artifacts/11872556802)

GitHub sign-in may be required to download artifacts. These artifacts currently expire on 2027-01-12; rebuild from the pinned dependencies if unavailable. See [Windows build and two-PC instructions](../WINDOWS.md) for runtime DLL placement and the Visual C++ runtime. Test `--software` first.

Deploy using [Appwrite instructions](../APPWRITE.md): add the private `limits` collection and optional request/receipt attributes, wait for indexes, deploy the Node 22 Function, then run the smoke script with two different real user JWTs. No schema or Function was deployed to the user's account by this task.

## Remaining external acceptance work

- **Appwrite Cloud:** real schema/transaction behavior, authentication/JWT expiry, Realtime subscriptions, scheduled cleanup, and the actual Education/Student plan quotas. No authorized Appwrite connection or credentials were available. Offline smoke success does not prove any of these.
- **Two Windows 11 PCs:** physical video/audio output, one-hour drift and congestion, and compatible NVENC/QSV/AMF hardware. Hosted Windows Server CI uses dummy devices.
- **Separate networks:** LAN and WAN/NAT/STUN/authorized UDP TURN, including forced relay and external packet loss/reordering. Same-machine ICE success does not establish Internet reliability.

Scope remains one host and one viewer, up to 1080p SDR, H.264/Opus, libdatachannel and FFmpeg. Congestion control remains a conservative prototype, UDP TURN only, and runtime encoder failure is reported rather than transparently switching encoders. No paid infrastructure or main-player repository changes were introduced. Appwrite carries coordination only; media stays in the encrypted live peer stream and viewer memory.


## New five-viewer prototype verification (pending external acceptance)

A later multi-viewer change adds configurable room capacity and one host WebRTC connection per viewer. The earlier single-viewer measurements above remain historical; they must not be interpreted as five-viewer validation. Run simultaneous WAN tests with five different devices and independently verify the selected ICE candidate pairs, viewer leave/rejoin isolation, host upload throughput, audio sync and TURN costs before calling this production-ready.

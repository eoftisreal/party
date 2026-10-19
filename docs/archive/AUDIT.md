> Archived historical record, not current setup instructions. See [the current contract](../PROTOCOL_V3.md) and current PR checks. Relative paths below describe the original record.

# Post-merge audit and verification

Audited `main` at `10e898f8946fde57209275f36c182d11b11aab69`, the merge of PR #1, on 2026-10-13. No later main commits were present. PR #3 (`fix/watch-party-audit-completion`) preserves all seven commits from unmerged PR #2 (`fix/reliable-signaling-and-windows`) and completes the follow-up audit. The user subsequently merged PR #3 into main as `4c8d1e90a4aca75c30ebd5cae17ce092473146bc`. The agent did not merge it. The FFmpeg/libdatachannel/Appwrite separation and existing working streaming implementation are preserved.

## Follow-up audit, 2026-10-14

At the start of this follow-up main still pointed to the PR #1 merge. After the user reported updating main, a fresh fetch confirmed the PR #3 merge. Its Git tree exactly matches tested commit `90c64c5f0e791d24fa324227991113c9bd806244`. The unmerged PR #2 was discovered, reviewed, and preserved rather than reimplemented. Its final CI run [38481494824](https://github.com/eoftisreal/party/actions/runs/38481494824) is now confirmed successful for backend, Linux ASan/UBSan, and Windows/MSVC, including real clean/impaired loopback and separate-process outage tests. This supersedes the historical pending-CI notes below.

Additional corrections in PR #3:

- Outgoing 429/INBOX_FULL, 503, transaction conflicts and transport failures now back off independently of incoming polling/ACKs/heartbeats. Previously a full shared inbox could deadlock negotiation. Four worker regressions require incoming application/ACK to unblock the outgoing retry and verify the same message ID is retained.
- A forward RTP sequence gap beyond the 1,024-packet reorder bound now resynchronizes the receiver and requests a keyframe on the application thread. Previously all subsequent packets could be rejected indefinitely. Regression covers recovery, removal of pre-gap fragments and delayed stale packets.
- SDP validation rejects contradictory/duplicate media directions, codec mappings placed in the wrong media section, multiple data channels and array-valued identifiers. No new media transport or download path is introduced.
- Windows two-PC instructions now point to this branch and use software encoding first.

Final source verification, CI links, measurements, and external requirements are recorded in [the completion report](verification/completion.md). The historical measurements below belong to PR #2 and are not relabeled as new runs. Appwrite Cloud, physical Windows 11 devices/GPU, and separate-network WAN/TURN tests remain unverified; no matching credentials or hardware were available.

## Confirmed defects and corrections

| Finding | Correction / regression evidence |
| --- | --- |
| Inbox messages deleted before application processing | Explicit processing result and post-apply ACK ledger; failed applications retry, lost ACK responses do not reapply SDP |
| ACK deletion destroyed send idempotency | Keep a SHA-256 receipt through the 120-second delivery TTL; reject conflicting reuse; backend regression and dropped-response executable test |
| Authorization event raced ahead of its authority snapshot, causing an endless restart loop after an outage | Refresh authority before publishing the event; event-consumption regression and real HTTP-outage scenario |
| Restart authorization retried with a new generation | Stable requestId across retries; server returns the same generation; backend and worker regressions |
| Delayed offers could replace current peers | Authoritative room connection snapshot and generation validation; stale inbox messages are deliberately discarded |
| ICE could be lost before SDP | Defer ACK/application until remote SDP exists; bounded retry and per-peer candidate deduplication; actual RTC test deliberately presents ICE before SDP and repeats descriptions |
| Lost expired-lease join responses could become terminal STALE_SESSION | Stable renewal request bound to the previous session; idempotent retry cannot steal another instance's session; backend/worker regressions |
| Remote viewer departure could close the host's room | Distinguish REMOTE_PEER_EXPIRED from own lease/session errors; queued-authorization regression and actual viewer rejoin |
| Network pauses overwrote desired host play state | Preserve user intent independently of actual source pause; reset replacement connection retry deadlines |
| Closed tracks could throw from pacer/feedback pumping and terminate the host | Convert transport exceptions/closed tracks to recoverable peer failure; actual RTC test closes the media track before replacing the peer |
| Delayed startup left audio permanently behind the bounded video queue, producing a black viewer | Trim startup/rebuffer audio to the live target and recover large clock lag; actual media test stalls output for 1.5 seconds while receiving frames |
| Pacer and partial RTP access units survived seeks | Clear unsent media and old receive epochs; reject stale packets before loss accounting; separate intentional-discard counter; repeated backward/forward seek tests |
| Missing clock anchors inflated jitter; best RTT stayed pinned indefinitely | Exclude unanchored transit, renew best-RTT window, bound clock adjustment, reset clocks on replacement; regressions |
| Callback targets could be released while callbacks were active | Peer close drains external frame/control callbacks; weak peer ownership is retained |
| Outage recovery reused a connected generation; HTTPS latency could extend client lease enforcement | Request a fresh generation after suspension and enforce a conservative atomic deadline from the application thread; 102-second real HTTP-outage regression added |
| Windows minimal FFmpeg lacked lavfi test input | Portable real raw-YUV/WAV generator fallback; production FFmpeg dependencies remain unchanged |
| Windows narrow argv could corrupt non-ASCII movie filenames | UTF-16 entry point converts arguments to UTF-8; Windows process test uses a Unicode host-side fixture filename |
| MSVC main CI failed with C3538 in rtp.cpp | Split mixed-type auto declarations, preserve MSVC C++20 build, initialize SDL explicitly for the custom main |
| Setup ignored incompatible existing schema/readiness; room creation/invite attempts lacked account budgets | Validate schema/index readiness, additive optional attributes and private limits collection; persisted budgets and cleanup regression |

Native signaling queues are bounded: 128 outgoing commands/events, 256 ledger entries, 64 ACKs/request, and 128 ICE candidates/peer. Application failure cannot turn into an ACK; transient HTTP failures retain the original identifiers. On heartbeat loss, applications pause/close media before lease expiry and retry coordination; an expired own lease rejoins, while a replaced session fails closed. JWT session authentication renews once after a 401. Realtime remains an optional wake-up hint; HTTP polling is authoritative.

## Verification recorded by the prior PR #2 task

Ubuntu 24.04 container, GCC 13, FFmpeg 6.1.1, pinned libdatachannel 0.24.1; generated 1920×1080/30fps movie with encoded audio. SDL dummy audio is used. No simulated media counters/frames substitute for decoding the real file.

- 22 backend tests pass, including actual node-appwrite SDK request-shape tests, authorization, transaction rollback/capacity fixtures, post-ACK replay, restart retries, departures, persisted budgets, schema checks and retention cleanup. Fixtures do not prove deployed transaction semantics.
- All three C++ test executables pass: core/clock, RTP loss/reordering/cutoff/hostile payloads, and signaling worker delivery/retry/outage/lease/session/queue regressions.
- Clean actual ICE/DTLS/SRTP streaming passes with pause/resume, forward/backward seeks, delayed output startup, SDP/ICE duplication, closed-track recovery, replacement peers and stop.
- Controlled RTP packet-loss/reordering streaming passes; NACK recovery is exercised on actual encrypted media. This is a sender-handler injection, not external network shaping.
- Three consecutive separate-process runs pass with the actual `party host` and `party viewer` executables, a private invitation, production backend routing/authorization in a local TLS fixture, real H.264/Opus playback, pause/resume, four seeks, viewer exit/rejoin and host closure. Each deliberately loses one successful signal HTTP response and one successful ACK response. Each expires the first synthetic JWT for both accounts and verifies the native HTTPS renewal/retry path. This is not live Cloud authentication. A later final run also passed the expanded deployment smoke script against the same local fixture ([report](verification/audit-two-process-final.json)); no Cloud smoke pass is claimed.
- Viewer process working directories remain empty. Code audit finds no viewer movie writer, download endpoint or movie transport through Appwrite. This does not constitute system-wide filesystem tracing.

### Measurements

The separate-process repetitions lasted 54.789 / 55.188 / 54.697 seconds. First viewers rendered 794 / 774 / 709 real frames; replacement viewers rendered 127 / 129 / 115 frames before host closure. Maximum observed A/V scheduling drift was **54.343ms** across those runs, with zero decoder errors. Rebuffer recovery was exercised; these are control/reconnect tests, not a steady-state 30fps benchmark or physical lip-sync calibration.

Total child CPU time was 30.186 / 28.783 / 25.657 seconds, including host, two sequential viewers, Node fixture and OpenSSL. Largest-child peak RSS was 158.0 / 166.8 / 158.2 MiB; it is **not summed concurrent RAM**. Raw reports: [run 1](verification/audit-two-process-1.json), [run 2](verification/audit-two-process-2.json), [run 3](verification/audit-two-process-3.json). [Clean loopback](verification/audit-loopback-clean.json) and [impaired loopback](verification/audit-loopback-impaired.json) include stream counters and combined-process resource measurements.

### CI and sanitizer status

The inspected main run [38456314108](https://github.com/eoftisreal/party/actions/runs/38456314108) passed backend/Linux and failed Windows compilation at `native/src/rtp.cpp:30` with MSVC C3538. PR #2's first Linux run independently reproduced the viewer-departure failure in the new separate-process test after passing compile/unit/clean/impaired media stages; subsequent fixes address that failure. The first PR Windows job built all MSVC executables, copied runtime DLLs and passed all three C++ test executables, then failed generating media because the minimal CLI lacks lavfi. A portable generator fallback now addresses that failure. Run 38469440005 passed backend and the full Linux sanitizer/media/separate-process job for f8727c3; its Windows job is still pending. The development environment disconnected during the subsequent outage test, so that interrupted test and fresh local sanitizer runs are not counted as passes. Latest CI results are linked in PR #2. A workflow definition or queued job is not a test pass.

## External requirements still outstanding

No authorized Appwrite connection or Appwrite credentials were available, so schema deployment, the live smoke script, Cloud transaction conflict behavior, Realtime notifications, real 15-minute JWT expiry/renewal, scheduled cleanup and Education/Student quotas were **not** verified. Setup/smoke scripts and [deployment migration instructions](APPWRITE.md) are ready; add the limits collection and optional receipt/requestId attributes before deploying the backend.

No physical Windows 11 machines, output devices, compatible GPU, two different Internet connections, authorized STUN/TURN infrastructure or external impairment router were available. Same-machine ICE is not proof of WAN/NAT connectivity. MSVC CI uses Windows Server 2022 and dummy SDL devices; physical Windows 11 rendering/audio, NVENC/QSV/AMF, LAN/WAN/relay-only traversal and a one-hour drift/congestion run remain external acceptance work. See [two-PC instructions](WINDOWS.md).

The congestion controller remains a conservative loss/jitter prototype rather than libwebrtc/GCC; resolution is fixed at up to 1080p, RTT-based reorder waiting is bounded at 250ms, UDP TURN only is supported, and runtime encoder failures are reported rather than transparently migrating encoders. Long outages pause and rebuild a connection. No main-player integration is changed.

## Reproduce the new executable path

```sh
npm --prefix backend ci
npm --prefix backend test
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
python3 tools/generate_media.py /tmp/party-test.mp4
python3 tools/run_loopback.py build/party_loopback /tmp/party-test.mp4
python3 tools/run_loopback.py build/party_loopback /tmp/party-test.mp4 --impair
python3 tools/two_process.py build/party /tmp/party-test.mp4 --report two-process.json
# Optional: real 102-second coordination outage and fresh-generation recovery
python3 tools/two_process.py build/party /tmp/party-test.mp4 --outage --report outage.json
```

OpenSSL and Node.js 22 are required for the TLS fixture. It binds only to loopback, uses temporary keys/synthetic account secrets, keeps TLS verification enabled, and never reuses developer credentials. Earlier intermediate executable failures are recorded above; they were used to find integration defects rather than treated as passing evidence.

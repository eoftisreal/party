# Streaming performance and quality

Rebuild the native player integration to use these changes. Redeploying the
Appwrite Function alone does not update the encoder, transport or player.

## SDR color accuracy

Party outputs **limited-range BT.709 H.264** in every quality profile. The host
now detects the decoded frame's YUV matrix and range (falling back to decoder
metadata, then BT.601 for untagged SD and BT.709 for untagged HD). BT.601,
full-range YUV, and RGB frames are explicitly transformed via an RGB
intermediate into BT.709 limited pixels **before** the encoder tags them.
Frames already in limited BT.709 retain the existing fast scaling path.
The player matches BT.709 in both SDL's direct YUV path and the RGB fallback.
Use `media.colorConvertedFrames` and `media.colorFormat` in host `stats` to
diagnose conversion activity.

This fixes incorrect matrix/range **interpretation**, not H.264 quantization,
display calibration, gamut/ICC management, or monitor-specific HDR settings.
It also does not implement HDR tone mapping or full wide-gamut primaries/
transfer-function conversion. The extra RGB step adds CPU and one 8-bit
round-trip for sources needing conversion; it is not used for ordinary
limited BT.709 movies. For a fair comparison, capture the **same decoded
frame** on the same calibrated SDR display at both endpoints rather than
judging two different monitors by eye.

## Host quality profiles

```
party host --file movie.mp4 --preview --quality high
```

| Profile | Maximum resolution | Initial video bitrate | Video bitrate ceiling |
| --- | --- | --- | --- |
| `data-saver` | 1280×720 | 1.5 Mbps | 3 Mbps |
| `balanced` (default) | 1920×1080 | 4 Mbps | 8 Mbps |
| `high` | 1920×1080 | 6 Mbps | 12 Mbps |

Resolution never exceeds the source. These are lossy H.264 SDR profiles, not
original-file transfer or HDR support. Allow additional bandwidth for audio,
RTP/encryption overhead and retransmission. Choose data-saver if upload capacity
is limited; choosing high cannot fix insufficient bandwidth. The existing
controller still reduces bitrate on loss/jitter and cautiously recovers.

The host announces its initial rate and ceiling over the encrypted control
channel. The receiver applies that configuration once per connection, so it
does not immediately force high-quality streams back to the old 5 Mbps ceiling.
Older peers remain compatible but can still impose their old rate limit.
`Peer::create` accepts optional initial/max bitrate arguments in bits/second;
pass the same rates in `MediaOptions` (new trailing `maxBitrate` field).
Rebuilt API callers using default options also receive the balanced 4/8 Mbps
profile. Integrations that explicitly pass the old rates must update those
values to opt into higher quality.

## Connection and playback behavior

- HTTPS connections, DNS results and TLS sessions are reused per worker thread.
  Each request resets its options, headers and body; certificate verification
  and the no-redirect policy remain enabled.
- Room/inbox polling runs every 250 ms during negotiation, returns to 2 seconds
  on connection, and automatically falls back after 35 seconds. The private
  combined /sync request replaces separate room/inbox polls. Integrators call `connecting(true)` when
  starting/replacing a peer and `connecting(false)` once connected. The burst
  temporarily increases Function/database traffic; it does not bypass auth,
  generation validation, ACKs, retry backoff or leases.
- Pacing permits up to 10 ms of bounded credit at the configured rate, so a
  5 ms application pump can sustain higher bitrates. Call `Peer::pump()` every
  2–5 ms off the UI thread.
- Encoder reductions of at least 10% can apply after one second. Increases of
  at least 20% are coalesced and apply no more than once per five seconds.
  This avoids repeated encoder reopening for tiny estimate changes. Encoder
  reopening still occurs for meaningful changes; seamless runtime hardware
  reconfiguration is not implemented.
- Audio arrival variation selects a 350–600 ms playback budget during startup
  or rebuffering. The budget stays fixed during uninterrupted playback to avoid
  clock jumps. Decoded video is bounded to 24 frames (previously 15) to cover
  this delay. Memory remains bounded, with up to nine extra decoded frames.
  Host preview and viewer may choose different budgets under network jitter;
  exact cross-device alignment still requires player-level coordination.
- The SDL reference renderer uploads limited-range BT.709 YUV420P directly to
  YUV textures when supported, avoiding a CPU RGB conversion and staging copy.
  Other formats/renderers retain an RGBA fallback with a reused allocation.
  A custom player renderer must apply its own equivalent optimization.

## Diagnostics

The viewer's playout target is selected at startup and rebuffering from both
smoothed inter-arrival variation and a decaying 95th-percentile histogram
(8 ms buckets, roughly 33-packet effective history). The existing 350–600 ms
movie-playout limits remain unchanged. During uninterrupted playback, the
buffer target stays fixed to avoid sudden clock jumps. This does **not** yet
add NetEQ, WSOLA, or Opus packet-loss concealment; those require independent
implementations and validation.

The application loop uses a process-local high-resolution waitable timer on
Windows when available, falling back to a normal waitable timer or standard
sleep. This improves scheduling precision but does not guarantee deadlines,
reduce OS/GPU render latency, or change the separate SDL audio device clock.

Use `stats` on both sides. Compare `roomReadyMs` (initial coordination/auth),
`mediaReadyMs` (coordination plus local media/output initialization), and `network.connectMs` (peer creation to ICE
connection), `player.firstVideoMs` (player creation/reset to first decoded video),
`player.playoutDelayUs`, `player.rebufferings`, `loop.maxIntervalUs`, `loop.lateIntervalsOver15ms`, `loop.highResolutionWaitableTimer`, `network.pacerQueueUs`,
`network.pacerDroppedPackets`, `media.encoder`, `media.activeBitrate`,
`media.bitrate` (requested), and `media.encoderOpens`. The first-frame metric
includes waiting for the host; it is not solely decoder processing time.

Faster polling cannot remove Appwrite cold starts or restrictive NAT/firewall
restrictions. Hardware decoder acceleration, original compressed-media transfer,
4K/HDR, production congestion control and dynamic resolution remain separate
work. This change does not claim a measured improvement on a user's WAN or GPU.

## Verification

The native unit tests cover bitrate limits and adaptive delay bounds; signaling
regressions cover burst/idle cadence alongside existing auth/retry recovery.
The real RTP loopback test can exercise host-negotiated high quality:

```sh
PARTY_TEST_HIGH_QUALITY=1 python3 tools/run_loopback.py build/linux-release/party_loopback /tmp/party-test.mp4 --window
```

CI also runs this high-quality scenario with ASan/UBSan, plus the existing normal,
impaired, seek/reconnect and separate-process TLS scenarios. Real Internet and
hardware encoder acceptance must be run on the target devices.

### Local verification, 2026-10-15

| Check | Result |
| --- | --- |
| Backend tests and syntax checks | 32 tests passed; syntax checks passed |
| Native core, RTP/pacer and signaling suites | All three passed |
| 1080p real RTP loopback | Passed pause, seeks, delayed output and replacement connection; zero decoder errors |
| High-quality windowed loopback | Passed at a 6 Mbps target; all 243 rendered frames used the YUV upload path; zero decoder errors |
| Impaired RTP loopback | Passed with 17 injected packet drops plus reordering; zero decoder errors |
| Separate host/viewer TLS scenario with outage | Passed in 163.4 seconds, including expired JWT renewal, lost signal/ACK responses, lease suspension/rejoin and viewer replacement; all process exit codes zero |

Native runs used AddressSanitizer and UndefinedBehaviorSanitizer. LeakSanitizer
was disabled locally because this container cannot inspect `/proc/.../task`;
leak detection is not claimed. The repository CI configuration retains its
normal sanitizer settings. The fixture uses synthetic authentication and an
in-memory database, not Appwrite Cloud. No comparative WAN startup-speed,
physical Windows display, hardware-encoder, or perceptual image-quality
measurement is claimed by these local results.

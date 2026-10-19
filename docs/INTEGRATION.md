# Integration with the existing C++20 player

See [the v3.1 contract and compatibility policy](PROTOCOL_V3.md). `PartyApi` sends the version header on bootstrap, adopts canonical `/v3` paths when metadata is advertised, and retains `/v1` for older account-free backends. `Signaling` uses batched exchange and bundled authorization snapshots when supported; it falls back to existing v3 requests automatically. Public worker events strip invitation and capability secrets.

`party_core` separates four areas:

| Component | Interface | Integration point |
| --- | --- | --- |
| Appwrite | `PartyApi`, `Signaling` | Set only Function URL; Start/Join directly issue capabilities; consume room/authorized/signal/rejoined/coordination-lost/closed events |
| Network | `Peer` | Create send-only host or receive-only viewer; process authenticated generations; feed local/remote SDP and ICE |
| Host media | `MediaSource` | Optional standalone FFmpeg reader/encoder; replace with your existing decoded-frame encode path |
| Viewer output | `Player` | Replace SDL rendering/audio with existing render/audio queues while retaining timing/recovery logic |

The standalone `party` executable is a reference orchestration loop, not an SDK singleton. Build `party_core` as a subdirectory or package target and keep networking callbacks off your GUI thread. `Peer::pump()` must run frequently (about every 2–5ms) to service packet pacing and reorder deadlines. Appwrite requests already run on a separate thread. For each signal event, check `Signaling::current(event)`, apply it, then call `processed(messageId, true)` only after success. `Peer::signal()` returns false for ICE without remote SDP; retain/retry using the delivery ledger instead of ACKing early. Failed processing calls `processed(messageId, false)`. Do not add a second deduplication cache that suppresses those retries. Superseded generations can be deliberately discarded after authority validation.

For host integration, encode your player's decoded frames as Annex-B H.264 with repeated SPS/PPS, IDRs on request, no B frames, and Opus stereo at 48kHz. Submit an `EncodedFrame` with the host **transport** presentation time in microseconds. Do not feed original movie bytes or reset transport time to zero on seek. Respond to `Peer::forceKeyframe`, `targetBitrate`, and transport failure statistics. The supplied `MediaSource` shows input timestamp normalization, FFmpeg conversion, resampling, hardware probing, source pacing, and encoder reconfiguration.

Publish a `PlaybackState` every 100ms and immediately on epoch changes. Epoch/cutoff changes need to precede newly encoded media; the reference source schedules a 150ms future transport origin and repeats state. Call `Peer::discardQueuedMedia()` when publishing a new epoch and feed the viewer's `Player::cutoffUs` into `Peer::receiveCutoffUs`. On seek, flush old demux/decode/encode work, drain or discard old audio, establish a new cutoff and force IDR recovery. Movie time remains a separate value for the UI. Pause/resume uses a new epoch to reject queued old packets. Rejoining uses a new backend session and connection generation, then starts at the current movie position.

For viewer integration, retain a bounded in-memory compressed queue, validate video dimensions, and wait for an IDR after reset. Place decoded audio on the player audio clock, schedule video against played audio timestamps, and apply gradual resampling correction for small drift. The supplied Player now selects a bounded 350–600ms budget at startup/rebuffering; expose playoutDelayUs and buffering statistics when integrating. Delay the host's local playback to the same stream presentation target if the host is watching too (`--preview` demonstrates this).

All peer callbacks capture weak ownership in the transport. Stop media production, close/reset Peer callbacks, stop signaling and release render/audio resources in that order. `Peer::close()` drains external callbacks before returning; keep targets alive until then. Preserve intended play state separately from network pauses. Handle coordination loss/rejoin by clearing peers and coordination restoration by negotiating a fresh generation **for each** missing native peer (`restart=true`), including viewers that recover later. A global already-restarted flag can reuse obsolete SDP/ICE authority for the remaining viewers. Ignore duplicate authorization events for an already-owned generation, and clear only the affected viewer's in-flight authorization on errors. Production integration should also address decoder isolation, bottleneck tests, dynamic resolution, physical clock calibration and subtitle/audio selection.

See [streaming tuning](STREAMING_TUNING.md) for the optional Peer/MediaOptions bitrate settings, negotiation polling lifecycle, and required native rebuild.

## Multiple viewers

The Appwrite room service now admits five viewers by default (`PARTY_MAX_VIEWERS=5`, valid 1–16). `GET /v1/rooms/{roomId}` provides a `connections` array for the host, containing the independently authorized `viewerId`, `generation`, and `state` for each viewer. Each viewer receives only its own entry in `connections` plus the backward-compatible singular `connection`. The host sees the singular `connection` only when exactly one connection exists.

Create one `Peer` for each viewer in a host-owned map keyed by viewer ID, with a separate ICE/SDP generation for each. The reference `party` app uses this model; it encodes a movie once and sends the same `EncodedFrame` to every connected peer. A viewer leaving, refreshing their session or requiring an ICE restart must not reset the other peers. The host owns play/pause/seek and broadcasts playback state and epoch changes to every connected viewer. Bitrate is conservatively capped at the lowest connected viewer's estimated rate because all viewers share a single encoding, rather than creating a private encoder for each.

**Capacity is not free.** Five viewers at 4 Mbps video each consume roughly 20 Mbps of host upload plus audio, RTP, retransmissions and ICE overhead. The standalone implementation is a small-group fan-out prototype, not an SFU. The main Windows player's integration must provide comparable lifetime management for independently connected peers. This architecture remains direct-only; restrictive network pairs may fail.

Server-only configuration must never be included in the Windows client. Separate-network, five-viewer streams and physical Windows output require live acceptance; see CAPABILITIES.md. No separate Libcoin player repository was available through the connected account, so only this standalone reference client is updated.

## Required Libcoin changes

- Replace AppwriteApi with PartyApi; remove login/guest bootstrap/JWT refresh, Appwrite SDK and endpoint/project/database client configuration. Keep only the Function URL.
- Start with one POST /v1/rooms; join with one POST /v1/rooms/:id/join after splitting roomId.inviteCode locally. Generate a private 256-bit requestKey and retain only for uncertain-request retries. Never log it.
- Keep partyToken/session in memory. PartyApi strips the bearer token from public return objects. Poll /sync and consume bundled STUN settings before peer authorization. Keep generation validation/ACK handling.
- On PEER_EXPIRED use the existing credential plus previousSession/stable requestId. Preserve the 100-second media-authority deadline, 30-minute reconnect window and terminal revoked/stale-session behavior.
- Keep media quality, color, clocks and per-viewer peer ownership. Add host revoke UI using POST /revoke if desired.
- `Signaling::authorize` returns its requestId. Track it per viewer and accept only matching authorized/error events; late results from a previous recovery must not clear newer work. Use `Signaling::current` for the latest generation authority, since queued room events can be older than the worker ledger.
- Rebuild both roles against v3; follow the coordinated deployment sequence in CAPABILITIES.md.

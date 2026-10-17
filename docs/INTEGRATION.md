# Integration with the existing C++20 player

`party_core` separates four areas:

| Component | Interface | Integration point |
| --- | --- | --- |
| Appwrite | `AppwriteApi`, `Signaling` | Set Function URL for automatic guest credentials (optional legacy session/JWT); consume room/authorized/signal/rejoined/coordination-lost/closed events |
| Network | `Peer` | Create send-only host or receive-only viewer; process authenticated generations; feed local/remote SDP and ICE |
| Host media | `MediaSource` | Optional standalone FFmpeg reader/encoder; replace with your existing decoded-frame encode path |
| Viewer output | `Player` | Replace SDL rendering/audio with existing render/audio queues while retaining timing/recovery logic |

The standalone `party` executable is a reference orchestration loop, not an SDK singleton. Build `party_core` as a subdirectory or package target and keep networking callbacks off your GUI thread. `Peer::pump()` must run frequently (about every 2–5ms) to service packet pacing and reorder deadlines. Appwrite requests already run on a separate thread. For each signal event, check `Signaling::current(event)`, apply it, then call `processed(messageId, true)` only after success. `Peer::signal()` returns false for ICE without remote SDP; retain/retry using the delivery ledger instead of ACKing early. Failed processing calls `processed(messageId, false)`. Do not add a second deduplication cache that suppresses those retries. Superseded generations can be deliberately discarded after authority validation.

For host integration, encode your player's decoded frames as Annex-B H.264 with repeated SPS/PPS, IDRs on request, no B frames, and Opus stereo at 48kHz. Submit an `EncodedFrame` with the host **transport** presentation time in microseconds. Do not feed original movie bytes or reset transport time to zero on seek. Respond to `Peer::forceKeyframe`, `targetBitrate`, and transport failure statistics. The supplied `MediaSource` shows input timestamp normalization, FFmpeg conversion, resampling, hardware probing, source pacing, and encoder reconfiguration.

Publish a `PlaybackState` every 100ms and immediately on epoch changes. Epoch/cutoff changes need to precede newly encoded media; the reference source schedules a 150ms future transport origin and repeats state. Call `Peer::discardQueuedMedia()` when publishing a new epoch and feed the viewer's `Player::cutoffUs` into `Peer::receiveCutoffUs`. On seek, flush old demux/decode/encode work, drain or discard old audio, establish a new cutoff and force IDR recovery. Movie time remains a separate value for the UI. Pause/resume uses a new epoch to reject queued old packets. Rejoining uses a new backend session and connection generation, then starts at the current movie position.

For viewer integration, retain a bounded in-memory compressed queue, validate video dimensions, and wait for an IDR after reset. Place decoded audio on the player audio clock, schedule video against played audio timestamps, and apply gradual resampling correction for small drift. The supplied Player now selects a bounded 350–600ms budget at startup/rebuffering; expose playoutDelayUs and buffering statistics when integrating. Delay the host's local playback to the same stream presentation target if the host is watching too (`--preview` demonstrates this).

All peer callbacks capture weak ownership in the transport. Stop media production, close/reset Peer callbacks, stop signaling and release render/audio resources in that order. `Peer::close()` drains external frame/control callbacks before returning; keep the callback targets alive until then. Preserve the user's intended play state separately from network-induced pauses, and reset the viewer clock when replacing a peer. Handle `coordination-lost` by pausing/closing media, `rejoined` by resetting session state, and `coordination-restored` by negotiating a fresh generation. The initial backend supports only one viewer; increasing membership alone does not implement fan-out or multi-peer congestion decisions. Production integration should also address decoder isolation, network bottleneck tests, dynamic resolution, device clock calibration, and subtitle/audio-track selection.

See [streaming tuning](STREAMING_TUNING.md) for the optional Peer/MediaOptions bitrate settings, negotiation polling lifecycle, and required native rebuild.

## Multiple viewers

The Appwrite room service now admits five viewers by default (`PARTY_MAX_VIEWERS=5`, valid 1–16). `GET /v1/rooms/{roomId}` provides a `connections` array for the host, containing the independently authorized `viewerId`, `generation`, and `state` for each viewer. Each viewer receives only its own entry in `connections` plus the backward-compatible singular `connection`. The host sees the singular `connection` only when exactly one connection exists.

Create one `Peer` for each viewer in a host-owned map keyed by viewer ID, with a separate ICE/SDP generation for each. The reference `party` app uses this model; it encodes a movie once and sends the same `EncodedFrame` to every connected peer. A viewer leaving, refreshing their session or requiring an ICE restart must not reset the other peers. The host owns play/pause/seek and broadcasts playback state and epoch changes to every connected viewer. Bitrate is conservatively capped at the lowest connected viewer's estimated rate because all viewers share a single encoding, rather than creating a private encoder for each.

**Capacity is not free.** Five viewers at 4 Mbps video each consume roughly 20 Mbps of host upload plus audio, RTP, retransmissions and ICE overhead. Any TURN-relayed viewer also consumes relay bandwidth. The standalone implementation is a small-group fan-out prototype, not an SFU. The main Windows player's integration must provide comparable lifetime management for independently connected peers. Larger groups or substantially different viewer networks may need an SFU and per-viewer simulcast adaptation.

Server-only `PARTY_MAX_VIEWERS` and `PARTY_TURN_SECRET` must never be included in the Windows client's environment, installer or source. Separate-network, five-viewer simultaneous streams, physical Windows output and TURN relay behavior still require live acceptance tests; passing local backend unit tests does not prove Internet-scale streaming.

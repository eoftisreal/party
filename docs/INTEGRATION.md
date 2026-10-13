# Integration with the existing C++20 player

`party_core` separates four areas:

| Component | Interface | Integration point |
| --- | --- | --- |
| Appwrite | `AppwriteApi`, `Signaling` | Supply user session/JWT from existing login; consume room/authorized/signal/closed events |
| Network | `Peer` | Create send-only host or receive-only viewer; process authenticated generations; feed local/remote SDP and ICE |
| Host media | `MediaSource` | Optional standalone FFmpeg reader/encoder; replace with your existing decoded-frame encode path |
| Viewer output | `Player` | Replace SDL rendering/audio with existing render/audio queues while retaining timing/recovery logic |

The standalone `party` executable is a reference orchestration loop, not an SDK singleton. Build `party_core` as a subdirectory or package target and keep networking callbacks off your GUI thread. `Peer::pump()` must run frequently (about every 2–5ms) to service packet pacing and reorder deadlines. Appwrite requests already run on a separate thread.

For host integration, encode your player's decoded frames as Annex-B H.264 with repeated SPS/PPS, IDRs on request, no B frames, and Opus stereo at 48kHz. Submit an `EncodedFrame` with the host **transport** presentation time in microseconds. Do not feed original movie bytes or reset transport time to zero on seek. Respond to `Peer::forceKeyframe`, `targetBitrate`, and transport failure statistics. The supplied `MediaSource` shows input timestamp normalization, FFmpeg conversion, resampling, hardware probing, source pacing, and encoder reconfiguration.

Publish a `PlaybackState` every 100ms and immediately on epoch changes. Epoch/cutoff changes need to precede newly encoded media; the reference source schedules a 150ms future transport origin and repeats state. On seek, flush old demux/decode/encode work, drain or discard old audio, establish a new cutoff and force IDR recovery. Movie time remains a separate value for the UI. Pause/resume uses a new epoch to reject queued old packets. Rejoining uses a new backend session and connection generation, then starts at the current movie position.

For viewer integration, retain a bounded in-memory compressed queue, validate video dimensions, and wait for an IDR after reset. Place decoded audio on the player audio clock, schedule video against played audio timestamps, and apply gradual resampling correction for small drift. Keep the 350ms prototype playout budget initially; expose jitter/buffering statistics before making it adaptive. Delay the host's local playback to the same stream presentation target if the host is watching too (`--preview` demonstrates this).

All peer callbacks capture weak ownership in the transport. Stop media production, close/reset Peer callbacks, stop signaling and release render/audio resources in that order. The initial backend supports only one viewer; increasing membership alone does not implement fan-out or multi-peer congestion decisions. Production integration should also address decoder isolation, network bottleneck tests, dynamic resolution, device clock calibration, and subtitle/audio-track selection.

# Coin Party protocol v3.1

The authoritative wire contract is [`backend/protocol/v3.json`](../backend/protocol/v3.json): endpoints, payloads, errors, signaling states, compatibility policy and bounds. The backend imports it at runtime; CMake generates the native protocol version and batch limits from it. Include `backend/protocol` in the Function package and retain that layout when packaging `party_core`.

## Compatibility

The canonical prefix is `/v3`. `/v1` remains an alias for the account-free protocol already used by Coin; it does not enable old account/JWT APIs. Missing version headers retain existing behavior. New clients send `X-Party-Protocol: 3` on their ordinary bootstrap request. Every response includes `protocol: {name: "coin-party", major: 3, minor: 1, features: ["sync-exchange", "peer-snapshot"]}` and `X-Party-Protocol: 3`. No discovery round trip is required; `GET /v3/protocol` is available for diagnostics. Unsupported explicit majors return HTTP 426 before any database work.

Minor releases add optional fields/endpoints/features only. Ignore unknown optional response fields, preserve existing field types, and enable optional operations only when advertised. Older v3 servers omit metadata: use `/v1`, GET `/sync`, individual `/signals`, `/ack` and `/heartbeat`. The native client follows this fallback automatically. A breaking release needs a new prefix and explicit opt-in; removing an alias/feature needs an announced supported migration. Do not silently change Coin's interpretation of an existing payload.

An existing bootstrap quirk is preserved: Start/Join `expiresAt` is numeric ICE Unix seconds. New `iceExpiresAt` repeats that value; `roomExpiresAt` is ISO8601 room expiry. Ordinary room snapshots retain ISO8601 `expiresAt`. Do not use the bootstrap's legacy `expiresAt` as a room lifetime.

## Reliable exchange

`POST /v3/rooms/{roomId}/sync` accepts optional `messages` (up to 8), `acknowledgements` (up to 32) and `heartbeat` (boolean). The whole JSON body is limited to 64 KiB, so split large SDPs into separate exchanges. Message payloads and membership/session authorization are identical to individual signaling requests.

ACK ownership, messages in input order and the optional heartbeat commit in one transaction with one room revision fence. A failure rolls back the complete exchange; `error.messageIndex`, when present, identifies the failed input message without echoing payloads or secrets. A caller can discard that invalid message and retry the remaining work. Auth/lease/room errors still require their normal recovery or termination handling.

The response contains the revalidated `room` snapshot and private `messages` inbox, plus `accepted` (original input IDs in order), `acknowledged: true` and `heartbeat` (whether requested). The inbox uses server-derived delivery IDs; use those IDs for ACKs, rather than original sender IDs. ACK only after successful application. Retry an uncertain response with the same message IDs and payloads. Fingerprint receipts outlive ACK deletion until their TTL; retrying cannot reapply the signal or double-charge its rate budget. A racing snapshot may fail after a commit, so an HTTP error also requires idempotent retry. Empty POST exchange and GET `/sync` are read-only. Receipt retention is two minutes, not indefinite deduplication; stale sessions/generations are never replayed.

The native worker prioritizes peer authorization over background coordination, uses the snapshot bundled in `/peers`, batches outgoing work and piggybacks heartbeats. Outbound backoff leaves read polling and ACK/heartbeat-only exchanges available: a full inbox must not roll back both directions' ACKs indefinitely. It retains post-application ACKs and original message IDs across response loss, bounds bodies/queues, and drops superseded outbound generations. Without the feature advertisement it keeps the existing v3 path.

## Contention and bounded work

Room mutation fences remain necessary for revocation/closure races. Batching shares one fence; transaction-local reads are cached and invalidated on writes. Root authorization reads are never cached. Host signaling looks up the target peer directly; a batch reads the queue once, accounts for its newly staged messages and persists its combined member rate count once. Closed generations do not consume live inbox capacity while awaiting cleanup. `/sync` recipient/session/expiry queries use an inbox index instead of fetching other viewers' messages.

Public start/join budgets use eight fixed shards with quotas summing to the original exact service cap. A request normally writes one shard; when full, it can spill into another. Fresh identities do not increase the total allowance or allocate unbounded counters. Saturation can still exhaust availability and cause more shard attempts; platform request/concurrency limits remain required. Per-room admission and per-member message limits are unchanged.

Room records retain at most 128 participant identities, including revoked tombstones. At the cap, a new identity receives `ROOM_MEMBERSHIP_LIMIT`; existing participants can continue and recover. This prevents repeated invitation entry from growing a room's authorization scans indefinitely.

Scheduled cleanup queries expired open and retained closed rooms separately, up to 32 each per invocation. Expiry closes authority immediately. Closed-room dependencies drain in transactions of at most 48 row deletions before the room fence and its admission counter are removed. Expired signaling and receipt cleanup each processes at most 64 records. Closure/revocation marks generations closed rather than deleting an entire room inbox in its critical transaction. Messages from closed/stale generations cannot be delivered; peer replacement also removes that pair's old queue. Schedule cleanup frequently enough for observed expiry volume and monitor backlog; one invocation is not a complete sweep.

## Rollout and verification

This is an additive upgrade from existing account-free v3. Run the idempotent server-only setup tool first against the existing `party-v2` database. It adds/checks `rooms.cleanup(status,expiresAt)`, `signals.inbox(roomId,to,toSession,expiresAt)`, `signals.expiry(expiresAt)` and `notifications.expiry(expiresAt)`, waiting for availability. Do not recreate collections or deploy before indexes are ready. Then deploy the backend with its contract JSON. Existing Coin requests continue on their alias; new native builds use advertised features. Rollback to the previous v3 backend is compatible with the native fallback. The earlier account-based v2-to-v3 migration remains a separate breaking migration in [CAPABILITIES.md](CAPABILITIES.md).

Backend tests cover protocol aliases/version rejection, authorization, capacity, exchange atomicity, ambiguous retries, budgets, indexed SDK queries and bounded cleanup. Windows CI builds the native target, runs native unit/media tests and exercises five independent viewers, dropped TLS responses and a 102-second coordination outage. Local fixtures do not establish Appwrite Cloud latency, deployed index behavior, physical output or WAN connectivity. No Cloud performance improvement is claimed without measurements on a staged deployment.

STUN-assisted direct WebRTC remains the only media path. Some NAT/firewall pairs cannot connect. If broader compatibility becomes a requirement, optional TURN would require an explicit product/security decision and a separate implementation; this release introduces no TURN configuration, relay or media server.

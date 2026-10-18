# Account-free, single-endpoint protocol (v3)

The client needs only `PARTY_FUNCTION_URL=https://party.sgp.appwrite.run`.
Appwrite endpoint, project, `party-v2`, collections and scoped runtime key stay on the Function.
No Accounts API, Appwrite session/JWT, global client API key, guest bootstrap or Realtime subscription exists in this protocol.

## Start, join and reconnect

1. Start: `POST /v1/rooms` with `{ "title": "Movie night", "requestKey": "64 random hex characters" }`, no Authorization header. Returns 201 with `roomId`, `inviteCode`, `partyToken`, `sessionId`, identity/role and STUN settings.
2. Display/share one invitation: `roomId.inviteCode` (32 hex + dot + 32 base64url characters). Do not make an invitation-bearing HTTP URL. Invitations never grant host authority.
3. Join: split the invitation locally, `POST /v1/rooms/:id/join` with `{ "inviteCode": "...", "requestKey": "a new private 64-hex value" }`, no Authorization header. Returns an independent viewer token/session and STUN settings. No additional authentication request.
4. Every subsequent operation sends `Authorization: Bearer party_<64-hex>` and `X-Party-Session: <32-hex>` to this Function only. Credentials live in memory. Do not print tokens or request keys, put them in URLs, persist them to settings, or send them to telemetry.
5. After `PEER_EXPIRED`, renew with the existing bearer token and `{ "previousSession": "...", "requestId": "32 random hex" }` at `/join`. Keep the same requestId until success. Renewal rotates the session/generations, not the capability. `STALE_SESSION`, revocation and room closure are terminal. An active lease cannot be stolen through renewal.

`requestKey` is optional for nonretrying callers. For safe retries generate 32 bytes with an OS CSPRNG, retain in memory only for that bootstrap operation and reuse after an uncertain response. The Function derives a domain-separated capability, room ID and invitation from it. Treat it as a secret equivalent to the resulting capability, not a public idempotency identifier. This allows retry without storing plaintext credentials or introducing a signing-key rotation dependency. Initial join retries return the same membership/session and cannot resurrect revoked memberships. A fresh start/join operation must use a fresh key. The native API retries uncertain bootstrap responses up to three attempts with bounded delays.

Tokens have 256 bits of entropy; invitations have 192. The database stores only a token-derived participant identity and an invitation SHA-256 hash. Authority is determined by current room membership and hostId, never by a supplied role/user ID. Bearer tokens are useful only in their room. The 120-second liveness lease, 30-minute host reconnect expiry, and revoked membership records bound access. A host heartbeat extends room expiry; viewer activity cannot. Restarting the application loses its credentials and is a new identity, not account recovery.

## Private coordination API

All room reads require the current session, not just a bearer token. All responses, including errors, use `Cache-Control: no-store`.

| Endpoint suffix | Method | Authorization and behavior |
| --- | --- | --- |
| (none) | GET | Membership and visible connection snapshot |
| `/sync` | GET | Snapshot plus private inbox in one revalidated read; zero transactions/writes |
| `/ice` | GET | Current public STUN settings for a live member |
| `/heartbeat` | POST | Refresh own membership; host also extends room expiry |
| `/peers` | POST | Host authorizes/restarts a single viewer generation; stable requestId makes retries idempotent |
| `/signals` | POST / GET | Authorized host offer / viewer answer / trickled direct ICE / disconnect, or own session inbox |
| `/ack` | POST | Acknowledge successfully applied messages only; receipt survives deletion for response-loss retry |
| `/revoke` | POST | Host only, body `{ "viewerId": "..." }`; closes that viewer's peers and tombstones credential |
| `/leave` | POST | Viewer revokes own membership; host closes party |
| `/close` | POST | Host only; closes party |

Base is `/v1/rooms/:roomId`. See source validators for exact SDP/message fields. Host and viewer signals are restricted to their authorized pair and current sessions/generation. A viewer cannot authorize streams, publish offers, close rooms or revoke others. Revocation retains an optional `members.revoked` string marker (`yes`) until room cleanup; old bootstrap keys cannot revive that identity. Revocation does not ban a human with a still-valid shared invitation from obtaining a *new* credential. Share invitations only with intended participants.

Native signaling polls `/sync` every 250 ms during a bounded 35-second negotiation window, then every 2 seconds, with 10-second heartbeats. TLS/DNS/connections are reused on the HTTP worker. Start/Join bundles STUN configuration and skips an immediate redundant heartbeat. Backoff, bounded queues, message/authorization IDs and delivery ACKs survive transient errors. There is no public Appwrite Realtime subscription. Polling still consumes Function and DB quota; these are architectural request reductions, not measured Cloud latency promises. Room mutations retain optimistic transactions and revision checks for correctness; shared-room write contention is not eliminated.

At 100 seconds without a confirmed heartbeat, cooperating native clients suspend media authority and keep retrying coordination for up to 30 minutes. Weak connectivity does not send `/leave`. Explicit End/normal host shutdown attempts closure; a crash or offline shutdown relies on expiry. Existing direct media cannot be remotely revoked without client cooperation: host snapshots and lease deadlines enforce revocation. Keep those checks in the player integration.

## Abuse boundaries

Default durable budgets: service-wide 120 starts/hour, 300 public join attempts/minute, 30 join/renewal attempts/minute/room (including incorrect invitations), 180 new signaling messages/minute/member, bounded per-recipient/room inboxes, and 64 KiB request bodies. Fresh identities cannot bypass shared public limits. Limits are stored in existing `limits`; do not disable the create budget on a public service. The shared budget also means an attacker can exhaust service availability: this is *not* a complete volumetric DDoS defense. Configure platform/edge request and concurrency limits before public launch. Do not trust client-supplied forwarded-IP/user headers as an abuse identity. Read polling has no per-member durable rate counter to avoid adding a transaction per poll; edge throttling must cover it. No claim of official-player-only enforcement is made.

## Deployment / migration (explicit operator action)

This is a breaking coordination protocol. Do not silently deploy it over active legacy sessions.

1. Keep `party-v2` and all existing collections. Back up configuration/data normally; do not delete/recreate the database.
2. Run the additive `backend/tools/setup.js` with a server-only setup key against the existing database. It adds optional `members.revoked` (string8, default empty), checks permissions/indexes and never deletes existing data. Do not send setup keys to clients.
3. Finish active parties in a maintenance window. Old JWT/guest credentials will no longer work after upgrade; rooms cannot be mapped securely to new tokens automatically. Rebuild both ends together. Rollback requires the matching old client/backend pair; new credentials will not work with old code.
4. Preserve Function endpoint/project/database/runtime scopes (`documents.read`/`documents.write` for the existing legacy Databases API). Keep Node 22, root `backend`, entry `src/main.js`, build `npm ci --omit=dev`. Allow unauthenticated HTTPS invocation so the application's invitation handler can execute. Accounts are not used for authorization.
5. Remove TURN settings. Optionally set server-only `PARTY_STUN_URLS` (one STUN URL, or `[]` for LAN testing), `PARTY_MAX_VIEWERS` (default 5), and positive `PARTY_CREATE_LIMIT_PER_HOUR` (default 120, service-wide). Configure edge limits. Keep scheduled cleanup; old account-readable notifications expire and new receipt documents have empty permissions. Audit old collection/document grants; no client should subscribe to them.
6. Deploy only after reviewing CI. Set only the Function URL in the player, start new rooms, run `node backend/tools/smoke.js` with that URL, then real two-network and five-viewer acceptance. Smoke creates and closes a test room, but does not test real media.

No live schema change or deployment is performed by this implementation. Local SDK contract fixtures are not a live Appwrite test. Historical files under `docs/verification` describe older commits, not this protocol's verification.

## Media and direct-only limitation

The media pipeline, quality presets, H.264/Opus, BT.709 conversion, clocks, playback controls, congestion adaptation and per-viewer peer statistics are preserved. The authentication change does not add transcoding or a media server. This is still a lossy live encode, not pixel-identical original-file playback.

Only STUN-assisted direct UDP WebRTC is supported. Backend and native configuration reject TURN; no SFU or relay is introduced. Some symmetric NAT/CGNAT/firewall combinations cannot connect. More signaling retries cannot solve every network restriction. Report failure honestly, keep the party available for reconnect, and do not promise universal cross-network connectivity.

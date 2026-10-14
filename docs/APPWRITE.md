# Appwrite deployment and Section 1 audit

## Audit changes

The original JWT verification and hashed invitations were retained. Changes correct the 100-member pagination truncation, unlimited room membership, join/close races, stale peer sessions, absent signaling, missing queue limits, and missing cleanup. Server writes use Appwrite's transaction API; there is no unsafe fallback to nontransactional writes. The original deployment guide's database-only scopes were insufficient for document operations.

## Fresh development schema

Use a separate development database (`party-v2` is suggested). The original pilot collections lack the new attributes. Do not delete an existing database or apply required attributes to populated legacy collections blindly. `backend/tools/setup.js` creates collections/attributes/indexes and checks collection permissions without deleting data. Existing incompatible attributes need manual migration; old client applications need the v2 session header.

Use the legacy `/databases`/collections/documents API supported by `node-appwrite@23.1.0`. Your deployment must support `createTransaction`, document `transactionId`, and commit/rollback. Cloud/self-hosted versions lacking these endpoints will fail; run the live smoke test before relying on the service.

With a short-lived **server-only setup key** having database, collection, attribute and index read/write permissions:

```sh
cd backend
npm ci
# Set APPWRITE_ENDPOINT, APPWRITE_PROJECT_ID, PARTY_DATABASE_ID=party-v2,
# and PARTY_SETUP_API_KEY in your shell; do not commit credentials.
node tools/setup.js
```

Setup now waits for attribute/index readiness and rejects incompatible existing types, requiredness, string capacities and integer ranges. It does not deploy or test media.

| Collection | Required attributes |
| --- | --- |
| rooms | hostId string36, title string80, inviteHash string64, status string12, expiresAt datetime, revision integer |
| members | roomId string32, userId string36, joinedAt/lastSeenAt datetime, sessionId string32, rateWindow/rateCount integer |
| peers | optional requestId string32 (default empty); roomId string32, hostId/viewerId string36, hostSession/viewerSession/generation string32, state string16, expiresAt datetime |
| signals | roomId/generation/toSession string32, from/to string36, type string16, payload string65536, expiresAt datetime |
| notifications | optional fingerprint string64 (default empty); roomId string32, userId string36, expiresAt datetime |
| limits | revision, createWindow/createCount, joinWindow/joinCount integers |

All collections have **no collection permissions**, with document security enabled. Members, peers, rooms and signals also have no document permissions. Only notification documents grant `read` to their recipient user; notifications disclose no signaling secrets; fingerprint is a SHA-256 delivery receipt, not the SDP/ICE payload. Create a key index on `roomId` in members, peers, signals and notifications.

## Upgrade from merged PR #1

Before deploying this backend, rerun `node tools/setup.js` against the **v2** database with the setup key. This adds the private `limits` collection and optional `peers.requestId`/`notifications.fingerprint` attributes, avoiding required-attribute changes on existing documents. Set `PARTY_LIMITS_COLLECTION_ID` if overriding names. Existing incompatible pilot/v1 collections still need a separate database migration. The tool fails on incompatible schemas and never deletes data. Deploy the backend after setup reports available indexes, update both native clients, then run the live smoke. Use a development project first; setup was verified offline, not against the user's Cloud account.

Appwrite transaction limits vary by plan; the room queue is capped at 64 and normal room operations stay below the documented free-plan 100-write transaction limit. Do not raise the queue limit without checking the plan. Scheduled cleanup scans collections; large installations need indexed expiry pagination and bounded batches.

## Function configuration

Create a Node.js 22 Function, root `backend`, entrypoint `src/main.js`, build command `npm ci --omit=dev`. Enable a Function HTTPS domain. The domain API verifies `Authorization: Bearer <Appwrite JWT>` independently of Appwrite's injected user identity. The HTTPS domain must accept requests so JWT authentication can execute; never trust a caller-provided user ID.

Set `PARTY_DATABASE_ID=party-v2`. Collection IDs default to the names above and can be overridden using the variables in `backend/.env.example`. Appwrite injects its endpoint, project and scoped runtime API key; never pass that runtime key to a host/viewer.

For the legacy document API, enable `documents.read` and `documents.write` in the Function's dynamic API key scopes. This is the deprecated Databases/collections compatibility API for TablesDB, not the newer schemaless DocumentsDB service. Current consoles may expose equivalent rows.read/rows.write scopes: use the scope names presented for the **legacy `/databases` API**, then run `tools/smoke.js` to verify actual permissions. The schema setup key needs additional collection/attribute/index scopes; the deployed Function does not change schemas.

Set a five-minute schedule for expiry/notification cleanup. Appwrite's platform-controlled `x-appwrite-trigger=schedule` invokes housekeeping; there is no public cleanup route. Closed rooms, memberships and peer records are removed one day after the original room expiry (rooms last 24 hours). Expired signaling receipts are removed after 120 seconds. Per-user abuse counters retain one private document/account; an administrative account-retention policy remains necessary at large scale. The setup script and scheduled cleanup have not been run against a live project in this development environment.

The client uses short Function requests; it does not hold an execution open as a WebSocket. Signaling stores and transmits no movie content. Polling plus transaction reads/writes still consumes Function/database quota. Measure usage on your actual student plan; free media transport does not make coordination or TURN unlimited.

## API

All paths except `/health` require an Appwrite user JWT. Streaming-sensitive endpoints also require `X-Party-Session`, returned by create/join. Rejoining rotates this session and revokes the old generation.

| Method/path | Body/behavior |
| --- | --- |
| POST /v1/rooms | `{ "title": "Movie night" }`; returns roomId, inviteCode, sessionId, yourUserId |
| POST /v1/rooms/:id/join | `{ "inviteCode": "..." }`; existing member may omit invite; automatic expired-lease renewal uses previousSession + requestId |
| GET /v1/rooms/:id | Current members, online status and authoritative connection generation/state; host receives session-version hashes |
| POST /v1/rooms/:id/heartbeat | Renews membership lease |
| POST /v1/rooms/:id/peers | Host only: `{ "viewerId": "...", "restart": false, "requestId": "32-hex" }`; returns generation |
| POST /v1/rooms/:id/signals | messageId, to, generation, type, payload; authenticated host offer, viewer answer, candidate or disconnect |
| GET /v1/rooms/:id/signals | Private inbox, current session/generation only |
| POST /v1/rooms/:id/ack | `{ "messageIds": ["..."] }`; removes recipient-owned messages |
| POST /v1/rooms/:id/leave | Viewer leaves; host departure closes room |
| POST /v1/rooms/:id/close | Host-only close |

Offer/answer payload is `{ "sdp": "..." }`; candidate payload is `{ "candidate": "candidate:...", "mid": "video" }`; disconnect payload is `{}`. MessageId/generation are 32 hex characters. SDP must advertise encrypted H.264 PT96 and Opus PT111, with host-sendonly/viewer-recvonly directions, ICE and SHA-256 DTLS fingerprint. Invitations are private bearer secrets. Native retries preserve messageId; a receipt survives ACK deletion until the 120-second TTL so lost HTTP responses do not replay an already applied offer. Reusing an identifier with different content is rejected. A new negotiation requires a fresh generation. The host preserves requestId across authorization retries, including restart=true, preventing lost responses from rotating the generation twice. Native inbox messages are ACKed only after successful application; candidates arriving before SDP remain pending. Superseded generations are deliberately discarded and ACKed after consulting the room snapshot.

## User authentication and Realtime

An existing player login integration can refresh a JWT file and set `PARTY_JWT_FILE`. A one-off `PARTY_JWT` also works but expires; JWTs normally last 15 minutes. For continuous native sessions, set the **user's** `PARTY_SESSION_SECRET`, endpoint and project. The native client requests `/account/jwts` using `X-Appwrite-Session` every ten minutes and keeps JWTs in memory. A Function 401 renews the JWT and retries once; revocation still fails closed. Membership lease expiration uses previousSession and a stable requestId. A lost renewal response can be retried without replacing the new session or stealing a newer instance's session; STALE_SESSION is terminal so two instances cannot repeatedly steal the same session. Coordination outages close/pause media after a conservative request-start deadline of 100 seconds without a successful heartbeat, retry for up to five minutes, and reconnect after authority is restored. Obtain the secret through your existing Appwrite login/token flow; no project API key belongs in the client.

Realtime uses `wss://<endpoint>/v1/realtime?project=<project>` and current official SDK `authentication`/`subscribe` messages. Session authentication is optional. If you supply only a JWT, the prototype uses authenticated Function polling rather than placing JWTs in a WebSocket URL. If an older deployed Realtime protocol differs, polling continues, but validate the subscription on your project before claiming Realtime tested. JWT files must be updated atomically by the login integration.

## Room-creation HTTP 500 on Appwrite 2.4.0

Room creation first commits a rate-limit transaction, then a room/host-membership transaction. The earlier backend sent `ttl: 30`, but Appwrite 2.4.0 validates TTL in `[60, 3600]` seconds. The server rejects transaction creation with `general_argument_invalid`; the public Function mapped that SDK error to HTTP 500. The backend now uses `ttl: 60`, retaining transaction isolation, commit/rollback and bounded conflict retries. No schema recreation or additional collection permissions are required for this correction.

Tagged 2.4.0 source: [transaction creation and documents.write scope](https://github.com/appwrite/appwrite/blob/2.4.0/src/Appwrite/Platform/Modules/Databases/Http/Databases/Transactions/Create.php), [TTL constants](https://github.com/appwrite/appwrite/blob/2.4.0/app/init/constants.php). `documents.read`/`documents.write` are the appropriate runtime scopes for the legacy API used here; setup collection/attribute scopes are separate. Other deployment mismatches remain possible and must be diagnosed from the actual error.

Redeploy `Party-Backend` from the fixed branch/commit using root `backend`, entrypoint `src/main.js`, and `npm ci --omit=dev`. Keep `PARTY_DATABASE_ID=party-v2` and the existing six collections. Regenerate the two user JWTs if expired, then rerun `node tools/smoke.js`. This fix was verified with SDK contract tests, not against the user's live Function.

If it still fails, open that execution's error log. A safe diagnostic looks like:

```json
{"event":"party_backend_error","operation":"transactions.create","status":400,"errorType":"general_argument_invalid","errorName":"AppwriteException"}
```

Operations identify `transactions.create`, `transactions.commit`, `transactions.rollback`, `account.get`, or `documents.<action>:<logical-collection>`. Error types/names use an allowlist; unrecognized types appear as `unknown`. Logs intentionally omit SDK messages, response bodies, stack traces, URLs, document identifiers, request data and credentials. A rollback failure does not replace the original failing-operation diagnostic. Share only this diagnostic line and the deployed commit when requesting help. Public responses remain generic.

## Live smoke

Use two different users and newly generated JWTs:

```sh
# PARTY_FUNCTION_URL, PARTY_HOST_JWT and PARTY_VIEWER_JWT in the environment
node tools/smoke.js
```

This creates and closes one test room and checks authentication, unauthorized joins, host authorization, transactions and closure. It does not prove media or Realtime. Run the separate Windows two-machine test afterward. Persisted account budgets allow four room creations/hour, 30 joins/minute including failed invitations, and 180 unique signals/minute/member; a room has at most 64 queued signals. Duplicate delivery retries do not spend the signal budget. Configure Appwrite project/domain abuse protection too: the application budgets do not prevent unauthenticated volumetric requests from consuming Function/authentication quota. Quotas of an Education/Student plan must be checked in that project; no paid service is required by this change.

Primary references: [JWT auth](https://appwrite.io/docs/products/auth/jwt), [legacy collection schema](https://appwrite.io/docs/products/databases/tablesdb/legacy/collections), [TablesDB transactions](https://appwrite.io/docs/products/databases/tablesdb/transactions), [official Web SDK Realtime protocol](https://github.com/appwrite/sdk-for-web/blob/main/src/client.ts), [Appwrite scope definitions](https://github.com/appwrite/appwrite/blob/main/app/config/scopes/project.php).

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

Wait for each index to become available. The tool does not deploy or test media.

| Collection | Required attributes |
| --- | --- |
| rooms | hostId string36, title string80, inviteHash string64, status string12, expiresAt datetime, revision integer |
| members | roomId string32, userId string36, joinedAt/lastSeenAt datetime, sessionId string32, rateWindow/rateCount integer |
| peers | roomId string32, hostId/viewerId string36, hostSession/viewerSession/generation string32, state string16, expiresAt datetime |
| signals | roomId/generation/toSession string32, from/to string36, type string16, payload string65536, expiresAt datetime |
| notifications | roomId string32, userId string36, expiresAt datetime |

All collections have **no collection permissions**, with document security enabled. Members, peers, rooms and signals also have no document permissions. Only notification documents grant `read` to their recipient user; notifications disclose no signaling secrets. Create a key index on `roomId` in members, peers, signals and notifications.

## Function configuration

Create a Node.js 22 Function, root `backend`, entrypoint `src/main.js`, build command `npm ci --omit=dev`. Enable a Function HTTPS domain. The domain API verifies `Authorization: Bearer <Appwrite JWT>` independently of Appwrite's injected user identity. The HTTPS domain must accept requests so JWT authentication can execute; never trust a caller-provided user ID.

Set `PARTY_DATABASE_ID=party-v2`. Collection IDs default to the names above and can be overridden using the variables in `backend/.env.example`. Appwrite injects its endpoint, project and scoped runtime API key; never pass that runtime key to a host/viewer.

For the legacy document API, enable `documents.read` and `documents.write` in the Function's dynamic API key scopes. Newer consoles may group these under DocumentsDB/database access: use the scope names presented for the **legacy `/databases` API**, then run `tools/smoke.js` to verify actual permissions. The schema setup key needs additional collection/attribute/index scopes; the deployed Function does not change schemas.

Set a five-minute schedule for expiry/notification cleanup. Appwrite's platform-controlled `x-appwrite-trigger=schedule` invokes housekeeping; there is no public cleanup route. Expired/closed room and membership records are retained for now, so apply an administrative retention policy before large-scale use. The setup script and scheduled cleanup have not been run against a live project in this development environment.

The client uses short Function requests; it does not hold an execution open as a WebSocket. Signaling stores and transmits no movie content. Polling plus transaction reads/writes still consumes Function/database quota. Measure usage on your actual student plan; free media transport does not make coordination or TURN unlimited.

## API

All paths except `/health` require an Appwrite user JWT. Streaming-sensitive endpoints also require `X-Party-Session`, returned by create/join. Rejoining rotates this session and revokes the old generation.

| Method/path | Body/behavior |
| --- | --- |
| POST /v1/rooms | `{ "title": "Movie night" }`; returns roomId, inviteCode, sessionId, yourUserId |
| POST /v1/rooms/:id/join | `{ "inviteCode": "..." }`; existing member may omit invite |
| GET /v1/rooms/:id | Current members and online status; host receives session-version hashes |
| POST /v1/rooms/:id/heartbeat | Renews membership lease |
| POST /v1/rooms/:id/peers | Host only: `{ "viewerId": "...", "restart": false }`; returns generation |
| POST /v1/rooms/:id/signals | messageId, to, generation, type, payload; authenticated host offer, viewer answer, candidate or disconnect |
| GET /v1/rooms/:id/signals | Private inbox, current session/generation only |
| POST /v1/rooms/:id/ack | `{ "messageIds": ["..."] }`; removes recipient-owned messages |
| POST /v1/rooms/:id/leave | Viewer leaves; host departure closes room |
| POST /v1/rooms/:id/close | Host-only close |

Offer/answer payload is `{ "sdp": "..." }`; candidate payload is `{ "candidate": "candidate:...", "mid": "video" }`; disconnect payload is `{}`. MessageId/generation are 32 hex characters. SDP must advertise encrypted H.264 PT96 and Opus PT111, with host-sendonly/viewer-recvonly directions, ICE and SHA-256 DTLS fingerprint. Invitations are private bearer secrets. SDP message IDs make retries idempotent while queued. Acknowledged messages may not be replayed to restart negotiations; create a new generation instead.

## User authentication and Realtime

An existing player login integration can refresh a JWT file and set `PARTY_JWT_FILE`. A one-off `PARTY_JWT` also works but expires; JWTs normally last 15 minutes. For continuous native sessions, set the **user's** `PARTY_SESSION_SECRET`, endpoint and project. The native client requests `/account/jwts` using `X-Appwrite-Session` every ten minutes and keeps JWTs in memory. Obtain the secret through your existing Appwrite login/token flow; no project API key belongs in the client.

Realtime uses `wss://<endpoint>/v1/realtime?project=<project>` and current official SDK `authentication`/`subscribe` messages. Session authentication is optional. If you supply only a JWT, the prototype uses authenticated Function polling rather than placing JWTs in a WebSocket URL. If an older deployed Realtime protocol differs, polling continues, but validate the subscription on your project before claiming Realtime tested. JWT files must be updated atomically by the login integration.

## Live smoke

Use two different users and newly generated JWTs:

```sh
# PARTY_FUNCTION_URL, PARTY_HOST_JWT and PARTY_VIEWER_JWT in the environment
node tools/smoke.js
```

This creates and closes one test room and checks authentication, unauthorized joins, host authorization, transactions and closure. It does not prove media or Realtime. Run the separate Windows two-machine test afterward. Configure project/function abuse protection before exposing room creation publicly; this prototype bounds each room but does not implement global/per-account room-creation quotas.

Primary references: [JWT auth](https://appwrite.io/docs/products/auth/jwt), [document transactions](https://appwrite.io/docs/products/databases/documentsdb/transactions), [official Web SDK Realtime protocol](https://github.com/appwrite/sdk-for-web/blob/main/src/client.ts), [Appwrite scope definitions](https://github.com/appwrite/appwrite/blob/main/app/config/scopes/project.php).

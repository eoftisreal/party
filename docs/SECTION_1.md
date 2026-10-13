# Section 1: Appwrite room backend

## Deploy
Create an Appwrite Databases (legacy API) database with ID `party`, and two collections:
- `rooms`: required `hostId` (string 36), `title` (string 80), `inviteHash` (string 64), `status` (string 12), `expiresAt` (datetime).
- `members`: required `roomId` (string 32), `userId` (string 36), `joinedAt` (datetime). Create a key index on roomId.

**No client read/write permissions for either collection.** Create an Appwrite Node.js 22 Function with root directory `backend`, entrypoint `src/main.js`, install command `npm install`, and `databases.read`/`databases.write` scopes. Set environment variables `PARTY_DATABASE_ID=party`, `PARTY_ROOMS_COLLECTION_ID=rooms`, `PARTY_MEMBERS_COLLECTION_ID=members`. Appwrite injects the project/endpoint and scoped runtime API key.

Clients authenticate with an Appwrite session and `account.createJWT()` and supply `Authorization: Bearer <JWT>`. The server verifies the JWT using Appwrite Account.get; never trust a user ID in request headers. Make the Function domain accessible only as intended and configure abuse/rate limits.

## API
- `GET /health` public health check
- `POST /v1/rooms` body `{"title":"Movie night"}`; returns `roomId` and one-time `inviteCode`
- `POST /v1/rooms/:roomId/join` body `{"inviteCode":"..."}`
- `GET /v1/rooms/:roomId` member-only details
- `POST /v1/rooms/:roomId/leave` leave; if host leaves, room closes
- `POST /v1/rooms/:roomId/close` host-only closure

Rooms expire after 24 hours. Backend stores only an SHA-256 invite hash. Video never travels through Appwrite Functions.

## Testing
`cd backend && npm test && npm run check`. Local unit tests do not require Appwrite, but a live deployment/smoke test is still required.

## Known limits
Pilot implementation only: member insertion vs room closure is not atomic; participant listing stops at 100; no capacity or quota enforcement, periodic cleanup, or room event broadcast yet. Do not deploy as a publicly unrestricted service. This section is the control plane, not the streaming plane.

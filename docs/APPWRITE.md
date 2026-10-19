# Appwrite deployment

The current coordination protocol is [account-free capabilities v3](CAPABILITIES.md). Read that guide for the complete API, security boundaries and mandatory migration sequence. JWT, guest bootstrap and Realtime integration are removed. Keep the existing `party-v2` database.

## Server-only setup

Use Node 22 and the pinned backend lockfile. Configure APPWRITE_ENDPOINT, APPWRITE_PROJECT_ID, PARTY_DATABASE_ID=party-v2 and a short-lived PARTY_SETUP_API_KEY only in your operator shell:

```sh
cd backend
npm ci
node tools/setup.js
```

Setup adds optional `members.revoked` (string8, default empty), retains collections/data, checks attribute compatibility and waits for indexes. It never deletes a database. Setup keys need collection/attribute/index administration scopes; never put one in the player.

For the additive v3.1 upgrade, run setup before deploying: it also creates/checks the recipient/session/expiry inbox and scheduled cleanup indexes listed in [PROTOCOL_V3.md](PROTOCOL_V3.md). Ship `backend/protocol/v3.json` alongside `src`. Cleanup is bounded and may require repeated scheduled invocations to drain a backlog. Existing account-free Coin clients remain compatible; the maintenance-window requirement below applies to older account-based deployments.

Collections use empty collection permissions and document security. New rooms, memberships, peers, signals, receipts and limits have empty document permissions. Old account-readable notifications are not used; audit them and let cleanup expire them.

## Function

Retain Party-Backend, Node 22, root `backend`, entry `src/main.js`, build `npm ci --omit=dev`. Preserve runtime endpoint/project/key injection and server-only PARTY_DATABASE_ID=party-v2. Document read/write scopes must match the existing legacy `/databases` API and its transactions. Setup and runtime keys need different permissions. HTTPS public invocation must reach the application's capability handler. Never trust a caller-supplied Appwrite user ID.

See backend/.env.example for server-only collection overrides, positive service-wide start budget, max viewers and STUN settings. Remove TURN variables before deployment. Configure platform/edge request and concurrency limits; application budgets are not a volumetric DDoS defense. Keep scheduled cleanup. Closed rooms and revocation tombstones are retained until one day after expiry; expired signals/receipts are removed.

## Verification

```sh
npm test
npm run check
# Explicitly targets the configured deployment and creates/closes a test room:
PARTY_FUNCTION_URL=https://party.sgp.appwrite.run node tools/smoke.js
```

Local SDK contract fixtures verify transaction TTL, private permissions, rollback and bounded conflict retries. They are not live deployment tests. Verify the actual deployment, five viewers, real network changes and physical Windows output before release. Do not deploy over active parties without a coordinated maintenance window.

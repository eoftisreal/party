# Section 1 — audited and extended

The room API is now v2 and participates in authenticated signaling. See [Appwrite deployment, schema and API](APPWRITE.md) for the current instructions, including transaction support, correct document scopes and session headers. The original Section 1 database schema and clients need migration; use a separate development database first.

Local backend tests: `cd backend && npm ci && npm test && npm run check`.
Live Appwrite deployment is not verified by local unit tests. Run `node tools/smoke.js` with two real user JWTs before testing native streaming remotely.

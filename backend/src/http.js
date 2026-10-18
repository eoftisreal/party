import { randomBytes } from 'node:crypto';
import { ApiError, digest } from './rooms.js';
import { iceSettings } from './ice.js';
// Participant tokens are private room capabilities. Persist only derived identities.
const participant = token => 'g_' + digest(token).slice(0, 32);
const issueToken = () => 'party_' + randomBytes(32).toString('hex');
const route = /^\/v1\/rooms\/([a-f0-9]{32})(?:\/(join|leave|close|heartbeat|peers|signals|ack|ice))?$/;
function body(req) {
  if (typeof req.bodyText === 'string' && Buffer.byteLength(req.bodyText) > 65536)
    throw new ApiError(413, 'BODY_TOO_LARGE', 'Body too large');
  let value;
  try {
    value = typeof req.bodyText === 'string' && req.bodyText.length ? JSON.parse(req.bodyText) : req.bodyJson ?? {};
    if (!value || typeof value !== 'object' || Array.isArray(value)) throw Error();
  } catch { throw new ApiError(400, 'INVALID_JSON', 'Expected JSON object'); }
  if (Buffer.byteLength(JSON.stringify(value)) > 65536) throw new ApiError(413, 'BODY_TOO_LARGE', 'Body too large');
  return value;
}
function header(req, name) { return Object.entries(req.headers || {}).find(([k]) => k.toLowerCase() === name)?.[1]; }
export function makeHandler({ service, signaling, getIceSettings = iceSettings(), logError = () => {} }) {
  return async ({ req, res }) => {
    try {
      const path = (req.path || '/').replace(/\/$/, '') || '/', method = (req.method || '').toUpperCase();
      if (path === '/health' && method === 'GET') return res.json({ ok: true, service: 'party-room-api', version: 2 }, 200);
      if (path !== '/v1/rooms' && !route.test(path)) throw new ApiError(404, 'NOT_FOUND', 'Route not found');
      const [, id, action] = route.exec(path) || [];
      const auth = header(req, 'authorization');
      const match = typeof auth === 'string' && /^Bearer (party_[a-f0-9]{64})$/.exec(auth);
      if (auth && !match) throw new ApiError(401, 'INVALID_TOKEN', 'Invalid party credential');
      const caller = match ? participant(match[1]) : null;
      const session = header(req, 'x-party-session');
      // First-time creation and invitation entry have no account/token bootstrap request.
      if (path === '/v1/rooms' && method === 'POST') {
        if (caller) throw new ApiError(400, 'CREDENTIAL_NOT_REQUIRED', 'Start a new party without an existing credential');
        const token = issueToken();
        const room = await service.create(participant(token), body(req));
        return res.json({ ...room, partyToken: token }, 201, { 'cache-control': 'no-store' });
      }
      if (id && method === 'POST' && action === 'join') {
        const input = body(req);
        const token = caller ? match[1] : issueToken();
        if (!caller && (input.previousSession !== undefined || input.requestId !== undefined))
          throw new ApiError(401, 'AUTH_REQUIRED', 'Existing credential required for renewal');
        const room = await service.join(id, participant(token), input.inviteCode, input);
        return res.json({ ...room, partyToken: token }, 200, { 'cache-control': 'no-store' });
      }
      if (!caller) throw new ApiError(401, 'AUTH_REQUIRED', 'Party credential required');
      if (id && method === 'GET' && action === 'ice') {
        const settings = await service.inspect(id, caller, session, async () => getIceSettings(id, user, session));
        return res.json(settings, 200, { 'cache-control': 'no-store' });
      }
      if (id && method === 'GET' && !action) return res.json(await service.read(id, caller, session), 200);
      if (id && method === 'GET' && action === 'signals') return res.json(await signaling.inbox(id, caller, session), 200);
      if (id && method === 'POST') {
        const input = body(req);
        if (['leave', 'close', 'heartbeat'].includes(action)) return res.json(await service[action](id, caller, session), 200);
        if (action === 'peers') return res.json(await signaling.connect(id, caller, session, input), 200);
        if (action === 'signals') return res.json(await signaling.send(id, caller, session, input), 202);
        if (action === 'ack') return res.json(await signaling.ack(id, caller, session, input), 200);
      }
      throw new ApiError(405, 'METHOD_NOT_ALLOWED', 'Method not allowed');
    } catch (e) {
      if (e instanceof ApiError) return res.json({ error: { code: e.code, message: e.message } }, e.status);
      logError(e);
      if (e.code === 409) return res.json({ error: { code: 'RETRY', message: 'Concurrent update; retry request' } }, 409);
      return res.json({ error: { code: 'INTERNAL_ERROR', message: 'Request failed' } }, 500);
    }
  };
}

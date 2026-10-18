import { randomBytes } from 'node:crypto';
import { ApiError, digest } from './rooms.js';
import { iceSettings } from './ice.js';
// Participant tokens are private room capabilities. Persist only derived identities.
const participant = token => 'g_' + digest(token).slice(0, 32);
const issueToken = () => 'party_' + randomBytes(32).toString('hex');
const route = /^\/v1\/rooms\/([a-f0-9]{32})(?:\/(join|leave|close|heartbeat|peers|signals|ack|ice|sync|revoke))?$/;
// Optional 256-bit private retry key. Never an identifier, URL field or log value.
function retryKey(input) {
  if (input.requestKey === undefined) return null;
  if (typeof input.requestKey !== 'string' || !/^[a-f0-9]{64}$/.test(input.requestKey))
    throw new ApiError(400, 'INVALID_REQUEST_KEY', 'Expected a private random 256-bit retry key');
  return input.requestKey;
}
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
    const json = (value, status) => res.json(value, status, { 'cache-control': 'no-store' });
    try {
      const path = (req.path || '/').replace(/\/$/, '') || '/', method = (req.method || '').toUpperCase();
      if (path === '/health' && method === 'GET') return json({ ok: true, service: 'party-room-api', version: 3 }, 200);
      if (path !== '/v1/rooms' && !route.test(path)) throw new ApiError(404, 'NOT_FOUND', 'Route not found');
      const [, id, action] = route.exec(path) || [];
      const auth = header(req, 'authorization');
      const match = typeof auth === 'string' && /^Bearer (party_[a-f0-9]{64})$/.exec(auth);
      // Function-domain ingress can supply its own Authorization header even
      // when the player sends none. Public Start/Join establish a capability;
      // transport credentials must not turn those endpoints into account auth.
      // Private operations still require a valid room token below, and renewal
      // still requires an existing capability rather than an invitation alone.
      const publicEntry = method === 'POST' && (path === '/v1/rooms' || action === 'join');
      if (auth && !match && !publicEntry) throw new ApiError(401, 'INVALID_TOKEN', 'Invalid party credential');
      const caller = match ? participant(match[1]) : null;
      const session = header(req, 'x-party-session');
      // First-time creation and invitation entry have no account/token bootstrap request.
      if (path === '/v1/rooms' && method === 'POST') {
        if (caller) throw new ApiError(400, 'CREDENTIAL_NOT_REQUIRED', 'Start a new party without an existing credential');
        const input = body(req), key = retryKey(input);
        const settings = getIceSettings(); // Reject bad deployment configuration before creating state.
        const token = key ? 'party_' + digest('create-token:' + key) : issueToken();
        const room = await service.create(participant(token), input, { budgetIdentity: 'public-create',
          ...(key ? { roomId: digest('create-room:' + key).slice(0, 32), inviteCode: Buffer.from(digest('create-invite:' + key), 'hex').subarray(0, 24).toString('base64url') } : {}) });
        return json({ ...room, partyToken: token, ...settings }, 201);
      }
      if (id && method === 'POST' && action === 'join') {
        const input = body(req);
        const settings = getIceSettings();
        const key = retryKey(input);
        const token = caller ? match[1] : key ? 'party_' + digest(`join-token:${id}:${key}`) : issueToken();
        if (!caller && (input.previousSession !== undefined || input.requestId !== undefined))
          throw new ApiError(401, 'AUTH_REQUIRED', 'Existing credential required for renewal');
        if (caller) {
          if (!input.previousSession) throw new ApiError(400, 'INVALID_RENEWAL', 'Use previous session and request identifier to reconnect');
        } else await service.limit('public-join', 'join', 300);
        const room = await service.join(id, participant(token), input.inviteCode, { ...input, bootstrapRetry: !caller && Boolean(key) });
        return json({ ...room, partyToken: token, ...settings }, 200);
      }
      if (!caller) throw new ApiError(401, 'AUTH_REQUIRED', 'Party credential required');
      if (id && method === 'GET' && action === 'ice') {
        const settings = await service.inspect(id, caller, session, async () => getIceSettings(id, caller, session));
        return json(settings, 200);
      }
      // Membership reads must not bypass session/lease validation.
      if (id && !session) throw new ApiError(403, 'STALE_SESSION', 'Current session required');
      if (id && method === 'GET' && !action) return json(await service.read(id, caller, session), 200);
      if (id && method === 'GET' && action === 'signals') return json(await signaling.inbox(id, caller, session), 200);
      if (id && method === 'GET' && action === 'sync') return json(await service.sync(id, caller, session, signaling.messages), 200);
      if (id && method === 'POST') {
        const input = body(req);
        if (['leave', 'close', 'heartbeat'].includes(action)) return json(await service[action](id, caller, session), 200);
        if (action === 'revoke') {
          if (typeof input.viewerId !== 'string' || !/^g_[a-f0-9]{32}$/.test(input.viewerId)) throw new ApiError(400, 'INVALID_VIEWER', 'Invalid viewer');
          return json(await service.revokeMember(id, caller, session, input.viewerId), 200);
        }
        if (action === 'peers') return json(await signaling.connect(id, caller, session, input), 200);
        if (action === 'signals') return json(await signaling.send(id, caller, session, input), 202);
        if (action === 'ack') return json(await signaling.ack(id, caller, session, input), 200);
      }
      throw new ApiError(405, 'METHOD_NOT_ALLOWED', 'Method not allowed');
    } catch (e) {
      if (e instanceof ApiError) return json({ error: { code: e.code, message: e.message } }, e.status);
      logError(e);
      if (e.code === 409) return json({ error: { code: 'RETRY', message: 'Concurrent update; retry request' } }, 409);
      return json({ error: { code: 'INTERNAL_ERROR', message: 'Request failed' } }, 500);
    }
  };
}

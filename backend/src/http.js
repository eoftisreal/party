import { ApiError } from './rooms.js';
const route = /^\/v1\/rooms\/([a-f0-9]{32})(?:\/(join|leave|close|heartbeat|peers|signals|ack))?$/;
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
export function makeHandler({ service, signaling, authenticate, logError = () => {} }) {
  return async ({ req, res }) => {
    try {
      const path = (req.path || '/').replace(/\/$/, '') || '/', method = (req.method || '').toUpperCase();
      if (path === '/health' && method === 'GET') return res.json({ ok: true, service: 'party-room-api', version: 2 }, 200);
      if (path !== '/v1/rooms' && !route.test(path)) throw new ApiError(404, 'NOT_FOUND', 'Route not found');
      const auth = header(req, 'authorization');
      const token = typeof auth === 'string' && /^Bearer ([A-Za-z0-9._~-]+)$/.exec(auth)?.[1];
      if (!token || token.length > 8192) throw new ApiError(401, 'AUTH_REQUIRED', 'Appwrite JWT required');
      let user;
      try { user = await authenticate(token); }
      catch (e) { if (e.code === 401 || e.code === 403) throw new ApiError(401, 'INVALID_TOKEN', 'Invalid JWT'); throw e; }
      if (!user) throw new ApiError(401, 'INVALID_TOKEN', 'Invalid JWT');
      if (path === '/v1/rooms' && method === 'POST') return res.json(await service.create(user, body(req)), 201);
      const [, id, action] = route.exec(path) || [], session = header(req, 'x-party-session');
      if (id && method === 'GET' && !action) return res.json(await service.read(id, user), 200);
      if (id && method === 'GET' && action === 'signals') return res.json(await signaling.inbox(id, user, session), 200);
      if (id && method === 'POST') {
        const input = body(req);
        if (action === 'join') return res.json(await service.join(id, user, input.inviteCode), 200);
        if (['leave', 'close', 'heartbeat'].includes(action)) return res.json(await service[action](id, user, session), 200);
        if (action === 'peers') return res.json(await signaling.connect(id, user, session, input), 200);
        if (action === 'signals') return res.json(await signaling.send(id, user, session, input), 202);
        if (action === 'ack') return res.json(await signaling.ack(id, user, session, input), 200);
      }
      throw new ApiError(405, 'METHOD_NOT_ALLOWED', 'Method not allowed');
    } catch (e) {
      if (e instanceof ApiError) return res.json({ error: { code: e.code, message: e.message } }, e.status);
      if (e.code === 409) return res.json({ error: { code: 'RETRY', message: 'Concurrent update; retry request' } }, 409);
      logError(e); return res.json({ error: { code: 'INTERNAL_ERROR', message: 'Request failed' } }, 500);
    }
  };
}

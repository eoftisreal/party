import { makeAppwriteStore, verifyUserJWT } from './store.js';
import { makeRoomsService } from './rooms.js';
import { makeSignalingService } from './signaling.js';
import { makeHandler } from './http.js';
export default async function main(context) {
  try {
    const store = makeAppwriteStore(process.env, context.req.headers?.['x-appwrite-key']);
    const rooms = makeRoomsService(store);
    // Only Appwrite's trusted execution trigger invokes housekeeping. Never expose it as an HTTP route.
    if (context.req.headers?.['x-appwrite-trigger'] === 'schedule') return context.res.json(await rooms.cleanup(), 200);
    return await makeHandler({ service: rooms, signaling: makeSignalingService(store, rooms),
      authenticate: token => verifyUserJWT(token, process.env), logError: e => context.error?.('Room API error: ' + (e?.name || 'Error')) })(context);
  } catch { context.error?.('Room API startup failure'); return context.res.json({ error: { code: 'SERVICE_UNAVAILABLE', message: 'Service not configured' } }, 503); }
}

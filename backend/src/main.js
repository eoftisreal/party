import { makeAppwriteStore, verifyUserJWT } from './store.js';
import { makeRoomsService } from './rooms.js';
import { makeSignalingService } from './signaling.js';
import { makeHandler } from './http.js';
import { logDiagnostic } from './diagnostics.js';
export default async function main(context) {
  try {
    const store = makeAppwriteStore(process.env, context.req.headers?.['x-appwrite-key']);
    const rooms = makeRoomsService(store);
    // Only Appwrite's trusted execution trigger invokes housekeeping. Never expose it as an HTTP route.
    if (context.req.headers?.['x-appwrite-trigger'] === 'schedule') return context.res.json(await rooms.cleanup(), 200);
    return await makeHandler({ service: rooms, signaling: makeSignalingService(store, rooms),
      authenticate: token => verifyUserJWT(token, process.env), logError: e => logDiagnostic(message => context.error?.(message), e) })(context);
  } catch (e) { logDiagnostic(message => context.error?.(message), e, 'configuration.load'); return context.res.json({ error: { code: 'SERVICE_UNAVAILABLE', message: 'Service not configured' } }, 503); }
}

import { makeAppwriteStore, verifyUserJWT } from './store.js';
import { makeRoomsService } from './rooms.js';
import { makeSignalingService } from './signaling.js';
import { makeHandler } from './http.js';
import { logDiagnostic } from './diagnostics.js';
export default async function main(context) {
  try {
    const store = makeAppwriteStore(process.env, context.req.headers?.['x-appwrite-key']);
    // Unlimited hosting during development unless a positive production cap is configured.
    const createLimitPerHour = process.env.PARTY_CREATE_LIMIT_PER_HOUR === undefined ? 0 : Number(process.env.PARTY_CREATE_LIMIT_PER_HOUR);
    const rooms = makeRoomsService(store, { createLimitPerHour });
    // Only Appwrite's trusted execution trigger invokes housekeeping. Never expose it as an HTTP route.
    if (context.req.headers?.['x-appwrite-trigger'] === 'schedule') return context.res.json(await rooms.cleanup(), 200);
    return await makeHandler({ service: rooms, signaling: makeSignalingService(store, rooms),
      authenticate: token => verifyUserJWT(token, process.env), logError: e => logDiagnostic(message => context.error?.(message), e) })(context);
  } catch (e) { logDiagnostic(message => context.error?.(message), e, 'configuration.load'); return context.res.json({ error: { code: 'SERVICE_UNAVAILABLE', message: 'Service not configured' } }, 503); }
}

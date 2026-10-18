import { makeAppwriteStore } from './store.js';
import { makeRoomsService } from './rooms.js';
import { makeSignalingService } from './signaling.js';
import { makeHandler } from './http.js';
import { logDiagnostic } from './diagnostics.js';
export default async function main(context) {
  try {
    const store = makeAppwriteStore(process.env, context.req.headers?.['x-appwrite-key']);
    // Public creation has a durable shared budget, independent of minted identities.
    const createLimitPerHour = process.env.PARTY_CREATE_LIMIT_PER_HOUR === undefined ? 120 : Number(process.env.PARTY_CREATE_LIMIT_PER_HOUR);
    const maxViewers = process.env.PARTY_MAX_VIEWERS === undefined ? 5 : Number(process.env.PARTY_MAX_VIEWERS);
    const rooms = makeRoomsService(store, { createLimitPerHour, maxViewers });
    // Only Appwrite's trusted execution trigger invokes housekeeping. Never expose it as an HTTP route.
    if (context.req.headers?.['x-appwrite-trigger'] === 'schedule') return context.res.json(await rooms.cleanup(), 200);
    return await makeHandler({ service: rooms, signaling: makeSignalingService(store, rooms),
      logError: e => logDiagnostic(message => context.error?.(message), e) })(context);
  } catch (e) { logDiagnostic(message => context.error?.(message), e, 'configuration.load'); return context.res.json({ error: { code: 'SERVICE_UNAVAILABLE', message: 'Service not configured' } }, 503); }
}

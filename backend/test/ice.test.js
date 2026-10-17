import test from 'node:test';
import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { iceSettings } from '../src/ice.js';
import { makeHandler } from '../src/http.js';
import { fixture } from './fixture.js';
const env = { PARTY_STUN_URLS: '["stun:relay.example:3478"]', PARTY_TURN_URLS: '["turn:relay.example:3478?transport=udp"]', PARTY_TURN_SECRET: 'test-only-secret-'.repeat(3) };
test('temporary credentials match coturn and bind to session, room, user and expiry', () => {
  let time = 1700000000000;
  const settings = iceSettings(env, () => time), a = settings('room', 'host', 'session'), turn = a.iceServers[1];
  assert.equal(a.expiresAt, 1700003600);
  assert.equal(a.turnConfigured, true);
  assert.match(turn.username, /^1700003600:[a-f0-9]{32}$/);
  assert.equal(turn.credential, createHmac('sha1', env.PARTY_TURN_SECRET).update(turn.username).digest('base64'));
  for (const args of [['other', 'host', 'session'], ['room', 'viewer', 'session'], ['room', 'host', 'new-session']])
    assert.notEqual(settings(...args).iceServers[1].credential, turn.credential);
  time += 3600000;
  assert.notEqual(settings('room', 'host', 'session').iceServers[1].credential, turn.credential);
  assert.equal(JSON.stringify(a).includes(env.PARTY_TURN_SECRET), false);
});
test('missing configuration is explicit and unsupported configuration fails closed', () => {
  assert.deepEqual(iceSettings({})('r', 'u', 's').iceServers, []);
  assert.equal(iceSettings({})('r', 'u', 's').turnConfigured, false);
  for (const change of [
    { PARTY_TURN_SECRET: '' }, { PARTY_TURN_SECRET: 'short' },
    { PARTY_TURN_URLS: 'bad-json' }, { PARTY_TURN_URLS: '{}' },
    { PARTY_TURN_URLS: '["turns:relay.example:443"]' },
    { PARTY_TURN_URLS: '["turn:relay.example:3478?transport=tcp"]' },
    { PARTY_TURN_URLS: '["turn:user:password@relay.example:3478"]' },
    { PARTY_STUN_URLS: '["stun:a","stun:b"]' },
    { PARTY_TURN_URLS: '["turn:a","turn:b","turn:c"]' },
    { PARTY_TURN_TTL_SECONDS: '0' }, { PARTY_TURN_TTL_SECONDS: 'Infinity' }
  ]) assert.throws(() => iceSettings({ ...env, ...change })('r', 'u', 's'), e => e.code === 'ICE_CONFIGURATION_ERROR');
});
test('ICE route requires authenticated live membership and current session, including guests', async () => {
  const f = fixture(); let minted = 0;
  const provider = iceSettings(env);
  const handler = makeHandler({ service: f.rooms, signaling: f.signaling, authenticate: async t => t,
    getIceSettings: (...args) => { ++minted; return provider(...args); } });
  async function call(method, path, token, session, input = {}) {
    let result;
    await handler({ req: { method, path, bodyJson: input, headers: { ...(token ? { authorization: `Bearer ${token}` } : {}), ...(session ? { 'x-party-session': session } : {}) } },
      res: { json: (body, status, headers) => result = { body, status, headers } } });
    return result;
  }
  const guest = (await call('POST', '/v1/guest')).body.guestToken;
  const room = (await call('POST', '/v1/rooms', guest)).body, base = `/v1/rooms/${room.roomId}`;
  for (const [token, session, status] of [[undefined, undefined, 401], ['stranger', room.sessionId, 403], [guest, undefined, 403], [guest, 'f'.repeat(32), 403]])
    assert.equal((await call('GET', base + '/ice', token, session)).status, status);
  assert.equal(minted, 0);
  const result = await call('GET', base + '/ice', guest, room.sessionId);
  assert.equal(result.status, 200);
  assert.equal(result.headers['cache-control'], 'no-store');
  assert.equal(result.body.turnConfigured, true);
  f.advance(120001);
  assert.equal((await call('GET', base + '/ice', guest, room.sessionId)).body.error.code, 'PEER_EXPIRED');
  await f.rooms.close(room.roomId, room.yourUserId, room.sessionId);
  assert.equal((await call('GET', base + '/ice', guest, room.sessionId)).status, 410);
});

import test from 'node:test';
import assert from 'node:assert/strict';
import { iceSettings, defaultStunUrls } from '../src/ice.js';
test('STUN discovery defaults to direct-only; explicit LAN configuration is supported', () => {
  const settings = iceSettings({})();// No identity or account required to derive public STUN settings.
  assert.deepEqual(settings.iceServers, defaultStunUrls.map(url => ({url})));
  assert.equal(settings.turnConfigured, false);
  assert.equal(JSON.stringify(settings).includes('credential'), false);
  assert.deepEqual(iceSettings({PARTY_STUN_URLS:'[]'})().iceServers, []);
});
test('TURN and malformed STUN configuration fail closed', () => {
  for (const env of [
    {PARTY_TURN_URLS:'["turn:relay.example:3478"]'}, {PARTY_TURN_SECRET:'secret'},
    {PARTY_RELAY_ONLY:'1'}, {PARTY_STUN_URLS:'bad-json'}, {PARTY_STUN_URLS:'{}'},
    {PARTY_STUN_URLS:'["stun:a","stun:b"]'}, {PARTY_STUN_URLS:'["turn:a"]'},
    {PARTY_STUN_URLS:'["stun:user:password@a"]'}
  ]) assert.throws(() => iceSettings(env)(), e => e.code === 'ICE_CONFIGURATION_ERROR');
});

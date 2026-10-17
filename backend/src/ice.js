import { createHmac } from 'node:crypto';
import { ApiError, digest } from './rooms.js';

// Address discovery only: this service never carries the movie.
export const defaultStunUrls = ['stun:stun.cloudflare.com:3478'];

// The bundled libjuice transport accepts one STUN and two UDP TURN entries.
export function iceSettings(env = process.env, now = Date.now) {
  return (roomId, user, session) => {
    const unavailable = () => { throw new ApiError(503, 'ICE_CONFIGURATION_ERROR', 'Invalid server ICE configuration'); };
    const parse = (raw, scheme, max) => {
      if (!raw) return [];
      let urls;
      try { urls = JSON.parse(raw); } catch { return unavailable(); }
      if (!Array.isArray(urls) || urls.length > max || urls.some(url => typeof url !== 'string' ||
          !new RegExp(`^${scheme}:(?:[a-zA-Z0-9.-]+|\\[[a-fA-F0-9:]+\\])(?::[0-9]{1,5})?(?:\\?transport=udp)?$`).test(url))) return unavailable();
      return urls;
    };
    // Undefined means normal Internet discovery. Explicit [] (or empty) opts out.
    const stun = parse(env.PARTY_STUN_URLS ?? JSON.stringify(defaultStunUrls), 'stun', 1);
    const turn = parse(env.PARTY_TURN_URLS, 'turn', 2);
    const ttl = Number(env.PARTY_TURN_TTL_SECONDS ?? 3600);
    if (!Number.isInteger(ttl) || ttl < 300 || ttl > 86400 ||
        (turn.length && (!env.PARTY_TURN_SECRET || env.PARTY_TURN_SECRET.length < 32)) ||
        (!turn.length && env.PARTY_TURN_SECRET)) return unavailable();
    const expiresAt = Math.floor(now() / 1000) + ttl;
    const username = `${expiresAt}:${digest(`${roomId}:${user}:${session}`).slice(0, 32)}`;
    const credential = turn.length ? createHmac('sha1', env.PARTY_TURN_SECRET).update(username).digest('base64') : '';
    return { iceServers: [...stun.map(url => ({ url })), ...turn.map(url => ({ url, username, credential }))],
      ttlSeconds: ttl, expiresAt, turnConfigured: turn.length > 0 };
  };
}

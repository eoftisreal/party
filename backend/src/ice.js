import { ApiError } from './rooms.js';

// Address discovery only: media always flows directly between participants.
export const defaultStunUrls = ['stun:stun.cloudflare.com:3478'];
export function iceSettings(env = process.env, now = Date.now) {
  return () => {
    const unavailable = () => { throw new ApiError(503, 'ICE_CONFIGURATION_ERROR', 'Invalid direct-only ICE configuration'); };
    if (env.PARTY_TURN_URLS || env.PARTY_TURN_SECRET || env.PARTY_RELAY_ONLY === '1') return unavailable();
    let urls;
    try { urls = JSON.parse(env.PARTY_STUN_URLS ?? JSON.stringify(defaultStunUrls)); } catch { return unavailable(); }
    if (!Array.isArray(urls) || urls.length > 1 || urls.some(url => typeof url !== 'string' ||
      !/^stun:(?:[a-zA-Z0-9.-]+|\[[a-fA-F0-9:]+\])(?::[0-9]{1,5})?$/.test(url))) return unavailable();
    return { iceServers: urls.map(url => ({ url })), ttlSeconds: 3600,
      expiresAt: Math.floor(now() / 1000) + 3600, turnConfigured: false };
  };
}

import { digest, idPattern, peerId, memberId, fail } from './rooms.js';

const userPattern = /^[A-Za-z0-9][A-Za-z0-9._-]{0,35}$/;
function object(value) { return value && typeof value === 'object' && !Array.isArray(value); }
function exact(value, keys) {
  if (!object(value) || Object.keys(value).some(k => !keys.includes(k))) fail(400, 'INVALID_SIGNAL', 'Unexpected signal fields');
}
export function validateSignal(input) {
  exact(input, ['messageId', 'to', 'generation', 'type', 'payload']);
  if (typeof input.messageId !== 'string' || !idPattern.test(input.messageId) ||
      typeof input.generation !== 'string' || !idPattern.test(input.generation) ||
      typeof input.to !== 'string' || !userPattern.test(input.to))
    fail(400, 'INVALID_SIGNAL', 'Invalid signal identifiers');
  const { type, payload } = input;
  if (type === 'offer' || type === 'answer') {
    exact(payload, ['sdp']);
    if (typeof payload.sdp !== 'string' || payload.sdp.length > 60000 || !payload.sdp.startsWith('v=0\r\n') ||
        !/^a=fingerprint:sha-256 (?:[A-Fa-f0-9]{2}:){31}[A-Fa-f0-9]{2}\r?$/m.test(payload.sdp) || !/^a=ice-ufrag:.+\r?$/m.test(payload.sdp) ||
        !/^a=ice-pwd:.+\r?$/m.test(payload.sdp)) fail(400, 'INVALID_SDP', 'WebRTC SDP with ICE and DTLS required');
    const sections = payload.sdp.split(/(?=^m=)/m).slice(1);
    if (sections.some(section => {
      if (/^m=(video|audio) /.test(section)) {
        const directions = [...section.matchAll(/^a=(sendonly|recvonly|sendrecv|inactive)\r?$/gm)];
        const codec = section.startsWith('m=video ') ? /^a=rtpmap:96 H264\/90000\r?$/mi : /^a=rtpmap:111 opus\/48000\/2\r?$/mi;
        return !/^m=(video|audio) \d+ UDP\/TLS\/RTP\/SAVPF /m.test(section) ||
          directions.length !== 1 || directions[0][1] !== (type === 'offer' ? 'sendonly' : 'recvonly') || !codec.test(section);
      }
      return !/^m=application \d+ UDP\/DTLS\/SCTP /m.test(section);
    }) || sections.filter(s => /^m=application /.test(s)).length > 1 ||
        sections.filter(s => /^m=video /.test(s)).length !== 1 || sections.filter(s => /^m=audio /.test(s)).length !== 1 ||
        !/^a=rtpmap:96 H264\/90000\r?$/mi.test(payload.sdp) || !/^a=rtpmap:111 opus\/48000\/2\r?$/mi.test(payload.sdp))
      fail(400, 'INVALID_SDP', 'Require host-send/viewer-receive encrypted audio and video');
  } else if (type === 'candidate') {
    exact(payload, ['candidate', 'mid']);
    if (typeof payload.candidate !== 'string' || payload.candidate.length > 2048 || /[\r\n]/.test(payload.candidate) ||
        !/^(a=)?candidate:[^\s]+ \d+ (udp|tcp) \d+ [^\s]+ \d+ typ (host|srflx|prflx|relay)( .*)?$/i.test(payload.candidate) ||
        typeof payload.mid !== 'string' || !/^[A-Za-z0-9_-]{1,32}$/.test(payload.mid)) fail(400, 'INVALID_ICE', 'Invalid ICE candidate');
  } else if (type === 'disconnect') {
    exact(payload, []);
  } else fail(400, 'INVALID_SIGNAL', 'Unsupported signal type');
  return input;
}
export function makeSignalingService(store, rooms) {
  async function messages(s, room, user, session) {
    const result = [], peers = new Map();
    for (const m of await s.listSignals(room.$id)) {
      if (m.to !== user || m.toSession !== session || Date.parse(m.expiresAt) <= rooms.now()) continue;
      const id = peerId(room.$id, user === room.hostId ? m.from : user);
      if (!peers.has(id)) peers.set(id, await s.getPeer(id));
      const p = peers.get(id);
      if (!p || p.generation !== m.generation || (p.state === 'closed' && m.type !== 'disconnect')) continue;
      result.push({ messageId: m.$id, from: m.from, generation: m.generation, type: m.type, payload: JSON.parse(m.payload) });
    }
    result.sort((a, b) => (a.type === 'candidate') - (b.type === 'candidate'));
    return { messages: result.slice(0, 64), serverTimeMs: rooms.now() };
  }
  async function peer(s, room, user, generation) {
    const viewer = user === room.hostId ? null : user;
    const candidates = viewer ? [await s.getPeer(peerId(room.$id, viewer))] : await s.listPeers(room.$id);
    const p = candidates.find(p => p?.generation === generation && p.state !== 'closed');
    if (!p) fail(409, 'STALE_GENERATION', 'Connection was replaced or closed');
    const host = await s.getMember(memberId(room.$id, p.hostId)), guest = await s.getMember(memberId(room.$id, p.viewerId));
    if (!host || !guest || host.revoked || guest.revoked || host.sessionId !== p.hostSession || guest.sessionId !== p.viewerSession ||
        [host, guest].some(m => Date.parse(m.lastSeenAt) + 120000 <= rooms.now())) fail(403, 'REMOTE_PEER_EXPIRED', 'Peer session expired');
    return p;
  }
  return {
    messages,
    async connect(id, user, session, input) {
      exact(input, ['viewerId', 'restart', 'requestId']);
      if(input.requestId !== undefined && (typeof input.requestId !== 'string' || !idPattern.test(input.requestId))) fail(400,'INVALID_PEER','Invalid request identifier');
      if (typeof input.viewerId !== 'string' || !userPattern.test(input.viewerId) || (input.restart !== undefined && typeof input.restart !== 'boolean'))
        fail(400, 'INVALID_PEER', 'Invalid viewer');
      return store.atomic(async s => {
        const { room } = await rooms.authorize(s, id, user, session);
        if (room.hostId !== user || input.viewerId === user) fail(403, 'HOST_ONLY', 'Only host can authorize streaming');
        const viewer = await s.getMember(memberId(id, input.viewerId));
        if (!viewer || viewer.revoked || Date.parse(viewer.lastSeenAt) + 120000 <= rooms.now()) fail(403, 'REMOTE_PEER_EXPIRED', 'Viewer offline');
        const pid = peerId(id, input.viewerId), previous = await s.getPeer(pid);
        if (input.requestId && previous?.requestId === input.requestId && previous.hostSession === session && previous.viewerSession === viewer.sessionId) {
          if(previous.state === 'closed') fail(409,'STALE_GENERATION','Connection already closed');
          return {generation:previous.generation,viewerId:input.viewerId};
        }
        if (!input.restart && previous?.state !== 'closed' && previous?.hostSession === session && previous?.viewerSession === viewer.sessionId)
          return { generation: previous.generation, viewerId: input.viewerId };
        for (const m of await s.listSignals(id)) if (m.to === input.viewerId || m.from === input.viewerId) await s.deleteSignal(m.$id);
        const data = { roomId: id, hostId: user, viewerId: input.viewerId, hostSession: session, viewerSession: viewer.sessionId,
          generation: rooms.token(), requestId: input.requestId || '', state: 'authorized', expiresAt: room.expiresAt };
        if (previous) await s.updatePeer(pid, data); else await s.createPeer(pid, data);
        return { generation: data.generation, viewerId: input.viewerId };
      });
    },
    async send(id, user, session, input) {
      validateSignal(input);
      return store.atomic(async s => {
        const { room, member } = await rooms.authorize(s, id, user, session);
        const sid = digest(`${id}:${session}:${input.messageId}`).slice(0, 32);
        const fingerprint = digest(JSON.stringify([input.to,input.generation,input.type,input.payload]));
        const receipt = await s.getReceipt(sid);
        if(receipt?.fingerprint && Date.parse(receipt.expiresAt)>rooms.now()) {
          if(receipt.fingerprint !== fingerprint) fail(409,'MESSAGE_REUSE','Message identifier reused');
          return {accepted:true,messageId:sid}; // Completed delivery stays idempotent after ACK deletion.
        }
        const p = await peer(s, room, user, input.generation);
        const other = user === p.hostId ? p.viewerId : p.hostId;
        if (input.to !== other) fail(403, 'INVALID_TARGET', 'Signals must go between authorized host and viewer');
        if ((input.type === 'offer' && user !== p.hostId) || (input.type === 'answer' && user !== p.viewerId))
          fail(403, 'HOST_ONLY', 'Viewer cannot send media offers');
        const payload = JSON.stringify(input.payload), existing = await s.getSignal(sid);
        if (existing) {
          if (existing.payload !== payload || existing.type !== input.type || existing.generation !== input.generation || existing.to !== input.to)
            fail(409, 'MESSAGE_REUSE', 'Message identifier reused');
          return { accepted: true, messageId: sid };
        }
        if ((input.type === 'offer' && p.state !== 'authorized') || (input.type === 'answer' && p.state !== 'offered'))
          fail(409, 'SIGNAL_ORDER', 'Invalid negotiation order');
        const window = Math.floor(rooms.now() / 60000), count = member.rateWindow === window ? member.rateCount : 0;
        if (count >= 180) fail(429, 'RATE_LIMIT', 'Too many signaling messages');
        const queued = await s.listSignals(id);
        for (const m of queued) if (Date.parse(m.expiresAt) <= rooms.now()) await s.deleteSignal(m.$id);
        const outstanding = queued.filter(m => Date.parse(m.expiresAt) > rooms.now());
        // A busy viewer cannot starve signaling for everyone else in the room.
        if (outstanding.filter(m => m.to === input.to).length >= 48 || outstanding.length >= 256)
          fail(429, 'INBOX_FULL', 'Peer inbox is full');
        await s.updateMember(memberId(id, user), { rateWindow: window, rateCount: count + 1 });
        const expiresAt = new Date(Math.min(rooms.now() + 120000, Date.parse(room.expiresAt))).toISOString();
        await s.createSignal(sid, { roomId: id, from: user, to: input.to, generation: p.generation, type: input.type, payload,
          toSession: user === p.hostId ? p.viewerSession : p.hostSession, expiresAt });
        // Private idempotency receipt only. No Appwrite user permissions or Realtime.
        await s.notify(sid, { roomId: id, userId: input.to, expiresAt, fingerprint });
        if (input.type === 'offer' || input.type === 'answer' || input.type === 'disconnect')
          await s.updatePeer(p.$id, { state: input.type === 'offer' ? 'offered' : input.type === 'answer' ? 'connected' : 'closed' });
        return { accepted: true, messageId: sid };
      });
    },
    async inbox(id, user, session) {
      // Read-only polls revalidate room revision and membership before responding.
      return rooms.inspect(id, user, session, (s, room) => messages(s, room, user, session));
    },
    async ack(id, user, session, input) {
      exact(input, ['messageIds']);
      if (!Array.isArray(input.messageIds) || input.messageIds.length > 64 || input.messageIds.some(x => typeof x !== 'string' || !idPattern.test(x)))
        fail(400, 'INVALID_ACK', 'Invalid acknowledgement');
      return store.atomic(async s => {
        const { room } = await rooms.authorize(s, id, user, session);
        for (const mid of input.messageIds) {
          const m = await s.getSignal(mid);
          if (m && (m.roomId !== id || m.to !== user || m.toSession !== session)) fail(403, 'INVALID_ACK', 'Not your message');
          if (m) await s.deleteSignal(mid);
        }
        return { acknowledged: true };
      });
    },
  };
}

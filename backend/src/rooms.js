import { randomBytes, createHash, timingSafeEqual } from 'node:crypto';
export class ApiError extends Error {
  constructor(status, code, message) { super(message); this.status = status; this.code = code; }
}
export const digest = s => createHash('sha256').update(s).digest('hex');
export const memberId = (room, user) => digest(`${room}:${user}`).slice(0, 32);
export const peerId = (room, user) => digest(`peer:${room}:${user}`).slice(0, 32);
export const reconnectWindowMs = 30 * 60 * 1000;
export const idPattern = /^[a-f0-9]{32}$/;
export function fail(status, code, message) { throw new ApiError(status, code, message); }
export function makeRoomsService(store, { now = () => Date.now(), random = randomBytes, createLimitPerHour = 0 } = {}) {
  if (!Number.isSafeInteger(createLimitPerHour) || createLimitPerHour < 0 || createLimitPerHour > 10000)
    throw Error('Invalid createLimitPerHour (expected integer 0..10000)');
  const token = () => random(16).toString('hex');
  const timestamp = () => new Date(now()).toISOString();
  async function get(s, id) {
    if (!idPattern.test(id || '')) fail(400, 'INVALID_ROOM_ID', 'Invalid room identifier');
    const r = await s.getRoom(id);
    if (!r) fail(404, 'ROOM_NOT_FOUND', 'Room not found');
    return r;
  }
  function active(r) {
    if (r.status !== 'open' || !Number.isFinite(Date.parse(r.expiresAt)) || Date.parse(r.expiresAt) <= now())
      fail(410, 'ROOM_CLOSED', 'Room closed or expired');
  }
  async function lockRoom(s, id) {
    const original = await get(s, id);
    await s.updateRoom(id, { revision: (original.revision || 0) + 1 });
    // Stage the write BEFORE authorization, then re-read the staged document.
    // Appwrite transactions are optimistic; an unstaged preliminary read is not a lock.
    return get(s, id);
  }
  async function authorize(s, id, user, session, requireSession = true, allowOffline = false) {
    const room = s.isTransaction ? await lockRoom(s, id) : await get(s, id); active(room);
    const m = await s.getMember(memberId(id, user));
    if (!m) fail(403, 'NOT_A_MEMBER', 'Not a room member');
    if (requireSession && (!idPattern.test(session || '') || session !== m.sessionId))
      fail(403, 'STALE_SESSION', 'Join again; session was replaced');
    if (requireSession && !allowOffline && Date.parse(m.lastSeenAt) + 120000 <= now()) fail(403, 'PEER_EXPIRED', 'Peer lease expired; join again');
    return { room, member: m };
  }
  async function touch(s, r) { await s.updateRoom(r.$id, { revision: (r.revision || 0) + 1 }); }
  function safe(r, ms, user, session) {
    return { roomId: r.$id, title: r.title, hostId: r.hostId, status: r.status, expiresAt: r.expiresAt,
      participants: ms.map(m => ({ userId: m.userId, joinedAt: m.joinedAt, online: Date.parse(m.lastSeenAt) + 120000 > now(), ...(r.hostId === user ? { sessionVersion: digest(m.sessionId || '') } : {}) })),
      yourUserId: user, yourRole: r.hostId === user ? 'host' : 'viewer', ...(session ? { sessionId: session } : {}), serverTimeMs: now() };
  }
  async function revoke(s, id, user) {
    for (const p of await s.listPeers(id)) if (p.hostId === user || p.viewerId === user) await s.updatePeer(p.$id, { state: 'closed' });
    for (const m of await s.listSignals(id)) if (m.from === user || m.to === user) await s.deleteSignal(m.$id);
  }
  async function limit(user, kind) {
    const max = kind === 'create' ? createLimitPerHour : 30;
    if (max === 0) return; // Development default: unlimited room creation; skips the extra Appwrite transaction.
    await store.atomic(async s => {
      const id=digest('limit:'+user).slice(0,32);
      const original=await s.getLimit(id);
      if(!original) await s.createLimit(id,{revision:0,createWindow:0,createCount:0,joinWindow:0,joinCount:0});
      else await s.updateLimit(id,{revision:original.revision+1});
      const current=await s.getLimit(id);
      const window=Math.floor(now()/(kind==='create'?3600000:60000));
      const count=current[kind+'Window']===window?current[kind+'Count']:0;
      if(count >= max) fail(429,'RATE_LIMIT','Too many room requests');
      await s.updateLimit(id,{[kind+'Window']:window,[kind+'Count']:count+1});
    });
  }
  return {
    authorize, active, touch, token, timestamp, now,
    async create(user, input = {}) {
      const title = input.title ?? 'Watch party';
      if (typeof title !== 'string' || !title.trim() || title.trim().length > 80) fail(400, 'INVALID_TITLE', 'Title must be 1 to 80 characters');
      await limit(user,'create');
      const id = token(), inviteCode = random(24).toString('base64url'), sessionId = token();
      return store.atomic(async s => {
        const r = await s.createRoom(id, { hostId: user, title: title.trim(), inviteHash: digest(inviteCode), status: 'open', revision: 0,
          expiresAt: new Date(now() + reconnectWindowMs).toISOString() });
        const m = { roomId: id, userId: user, joinedAt: timestamp(), lastSeenAt: timestamp(), sessionId, rateWindow: 0, rateCount: 0 };
        await s.createMember(memberId(id, user), m);
        return { ...safe(r, [m], user, sessionId), inviteCode };
      });
    },
    async join(id, user, invite, renewal = {}) {
      const renewing=renewal.previousSession!==undefined || renewal.requestId!==undefined;
      if(renewing && (!idPattern.test(renewal.previousSession||'') || !idPattern.test(renewal.requestId||'')))
        fail(400,'INVALID_RENEWAL','Previous session and request identifier required');
      await limit(user,'join'); // Failed invite attempts also consume the committed budget.
      return store.atomic(async s => {
        const r = await lockRoom(s, id); active(r);
        const mid = memberId(id, user), existing = await s.getMember(mid);
        const renewed=renewing?digest(`renew:${id}:${user}:${renewal.previousSession}:${renewal.requestId}`).slice(0,32):null;
        if(renewing) {
          if(!existing)fail(403,'NOT_A_MEMBER','Membership no longer exists');
          if(existing.sessionId===renewed && Date.parse(existing.lastSeenAt)+120000>now()) {
            await s.updateMember(mid,{lastSeenAt:timestamp()});await touch(s,r);
            return safe(r,await s.listMembers(id),user,renewed);
          }
          if(existing.sessionId!==renewal.previousSession && existing.sessionId!==renewed)
            fail(403,'STALE_SESSION','Session was replaced by another instance');
          if(existing.sessionId===renewal.previousSession && Date.parse(existing.lastSeenAt)+120000>now())
            fail(409,'LEASE_ACTIVE','Membership lease has not expired');
        }
        if (!existing) {
          if (typeof invite !== 'string' || !/^[A-Za-z0-9_-]{32}$/.test(invite)) fail(403, 'INVALID_INVITE', 'Invite code required');
          const a = Buffer.from(digest(invite), 'hex'), b = Buffer.from(r.inviteHash, 'hex');
          if (a.length !== b.length || !timingSafeEqual(a, b)) fail(403, 'INVALID_INVITE', 'Invalid invite');
          const members = await s.listMembers(id);
          // One host + one viewer; stale viewers are explicitly removed before admitting another.
          for (const m of members) if (m.userId !== r.hostId && Date.parse(m.lastSeenAt) + reconnectWindowMs <= now()) {
            await revoke(s, id, m.userId); await s.deleteMember(memberId(id, m.userId));
          }
          if ((await s.listMembers(id)).length >= 2) fail(409, 'ROOM_FULL', 'Prototype supports one viewer');
        } else await revoke(s, id, user);
        const sessionId = renewed || token();
        const m = { roomId: id, userId: user, joinedAt: existing?.joinedAt || timestamp(), lastSeenAt: timestamp(), sessionId, rateWindow: 0, rateCount: 0 };
        if (existing) await s.updateMember(mid, m); else await s.createMember(mid, m);
        if (r.hostId === user) {
          r.expiresAt = new Date(now() + reconnectWindowMs).toISOString();
          await s.updateRoom(id, { expiresAt: r.expiresAt });
        }
        await touch(s, r);
        return safe(r, await s.listMembers(id), user, sessionId);
      });
    },
    async read(id, user, session) {
      return store.atomic(async s => {
        const {room}=await authorize(s,id,user,session,Boolean(session));
        const members=await s.listMembers(id);
        const peers=await s.listPeers(id);
        const p=peers.find(p=>members.some(m=>m.userId===p.hostId && m.sessionId===p.hostSession) &&
          members.some(m=>m.userId===p.viewerId && m.sessionId===p.viewerSession));
        const connection=p?{generation:p.generation,viewerId:p.viewerId,state:members.every(m=>Date.parse(m.lastSeenAt)+120000>now())?p.state:'closed'}:null;
        return {...safe(room,members,user),connection};
      });
    },
    async heartbeat(id, user, session) {
      return store.atomic(async s => {
        const { room } = await authorize(s, id, user, session);
        await s.updateMember(memberId(id, user), { lastSeenAt: timestamp() });
        if (room.hostId === user) await s.updateRoom(id, { expiresAt: new Date(now() + reconnectWindowMs).toISOString() });
        await touch(s, room);
        return { ok: true, serverTimeMs: now() };
      });
    },
    async leave(id, user, session) {
      return store.atomic(async s => {
        const { room } = await authorize(s, id, user, session, true, true);
        await revoke(s, id, user);
        if (room.hostId === user) await s.updateRoom(id, { status: 'closed', revision: (room.revision || 0) + 1 });
        else { await s.deleteMember(memberId(id, user)); await touch(s, room); }
        return { left: true, roomClosed: room.hostId === user };
      });
    },
    async close(id, user, session) {
      return store.atomic(async s => {
        const { room } = await authorize(s, id, user, session, true, true);
        if (room.hostId !== user) fail(403, 'HOST_ONLY', 'Only host can close');
        await revoke(s, id, user); await s.updateRoom(id, { status: 'closed', revision: (room.revision || 0) + 1 });
        return { closed: true };
      });
    },
    async cleanup() {
      // Scheduled invocation only, never routed through the public HTTP API.
      for (const r of await store.listRooms()) if (r.status === 'open' && Date.parse(r.expiresAt) <= now()) {
        await store.atomic(async s => {
          const fresh = await get(s, r.$id);
          await revoke(s, r.$id, fresh.hostId);
          await s.updateRoom(r.$id, { status: 'closed', revision: (fresh.revision || 0) + 1 });
        });
      }
      // Closed rooms are retained until one day after their original expiry for diagnostics.
      // Transactionally delete all dependent records; never prune an active room.
      for (const r of await store.listRooms()) if(r.status==='closed' && Date.parse(r.expiresAt)+86400000<=now()) {
        await store.atomic(async s=>{
          const fresh=await lockRoom(s,r.$id);
          if(fresh.status!=='closed' || Date.parse(fresh.expiresAt)+86400000>now())return;
          for(const m of await s.listSignals(r.$id))await s.deleteSignal(m.$id);
          for(const p of await s.listPeers(r.$id))await s.deletePeer(p.$id);
          for(const m of await s.listMembers(r.$id))await s.deleteMember(m.$id);
          await s.deleteRoom(r.$id);
        });
      }
      for (const r of await store.listRooms()) for (const m of await store.listSignals(r.$id))
        if (Date.parse(m.expiresAt) <= now()) { try { await store.deleteSignal(m.$id); } catch (e) { if (e.code !== 404) throw e; } }
      for (const n of await store.listNotifications()) if (Date.parse(n.expiresAt) <= now()) {
        try { await store.deleteNotification(n.$id); } catch (e) { if (e.code !== 404) throw e; }
      }
      return { cleaned: true };
    },
  };
}

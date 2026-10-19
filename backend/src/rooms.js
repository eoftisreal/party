import { randomBytes, createHash, timingSafeEqual } from 'node:crypto';
import { limits } from './protocol.js';
export class ApiError extends Error {
  constructor(status, code, message) { super(message); this.status = status; this.code = code; }
}
export const digest = s => createHash('sha256').update(s).digest('hex');
export const memberId = (room, user) => digest(`${room}:${user}`).slice(0, 32);
export const peerId = (room, user) => digest(`peer:${room}:${user}`).slice(0, 32);
export const reconnectWindowMs = 30 * 60 * 1000;
export const idPattern = /^[a-f0-9]{32}$/;
export function fail(status, code, message) { throw new ApiError(status, code, message); }
export function makeRoomsService(store, { now = () => Date.now(), random = randomBytes, createLimitPerHour = 120, maxViewers = 5 } = {}) {
  if (!Number.isSafeInteger(createLimitPerHour) || createLimitPerHour < 0 || createLimitPerHour > 10000)
    throw Error('Invalid createLimitPerHour (expected integer 0..10000)');
  if (!Number.isSafeInteger(maxViewers) || maxViewers < 1 || maxViewers > 16)
    throw Error('Invalid maxViewers (expected integer 1..16)');
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
    // Atomic increment avoids computing a revision from a stale preliminary read.
    if (!idPattern.test(id || '')) fail(400, 'INVALID_ROOM_ID', 'Invalid room identifier');
    try { await s.bumpRoom(id); } catch (e) { if (e.code === 404) fail(404, 'ROOM_NOT_FOUND', 'Room not found'); throw e; }
    // Stage the write BEFORE authorization, then re-read the staged document.
    // Appwrite transactions are optimistic; an unstaged preliminary read is not a lock.
    return get(s, id);
  }
  async function authorize(s, id, user, session, requireSession = true, allowOffline = false) {
    const room = s.isTransaction ? await lockRoom(s, id) : await get(s, id); active(room);
    const m = await s.getMember(memberId(id, user));
    if (!m) fail(403, 'NOT_A_MEMBER', 'Not a room member');
    if (m.revoked) fail(403, 'CREDENTIAL_REVOKED', 'Membership was revoked');
    if (requireSession && (!idPattern.test(session || '') || session !== m.sessionId))
      fail(403, 'STALE_SESSION', 'Join again; session was replaced');
    if (requireSession && !allowOffline && Date.parse(m.lastSeenAt) + 120000 <= now()) fail(403, 'PEER_EXPIRED', 'Peer lease expired; join again');
    return { room, member: m };
  }
  async function inspect(id, user, session, work, requireSession = true) {
    // Polls never stage writes or commit a transaction. Revalidate after collecting
    // the response so closure/session replacement cannot leak a stale inbox.
    for (let attempt = 0; attempt < 4; ++attempt) {
      const before = await authorize(store, id, user, session, requireSession);
      const result = await work(store, before.room);
      const after = await authorize(store, id, user, session, requireSession);
      if (before.room.revision === after.room.revision && before.room.$updatedAt === after.room.$updatedAt &&
          before.member.sessionId === after.member.sessionId) return result;
    }
    fail(409, 'RETRY', 'Room changed while reading; retry request');
  }
  function safe(r, ms, user, session) {
    return { roomId: r.$id, title: r.title, hostId: r.hostId, status: r.status, expiresAt: r.expiresAt, maxViewers,
      participants: ms.filter(m => !m.revoked).map(m => ({ userId: m.userId, joinedAt: m.joinedAt, online: Date.parse(m.lastSeenAt) + 120000 > now(), ...(r.hostId === user ? { sessionVersion: digest(m.sessionId || '') } : {}) })),
      yourUserId: user, yourRole: r.hostId === user ? 'host' : 'viewer', ...(session ? { sessionId: session } : {}), serverTimeMs: now() };
  }
  async function revoke(s, id, user) {
    for (const p of await s.listPeers(id)) if (p.state !== 'closed' && (p.hostId === user || p.viewerId === user)) await s.updatePeer(p.$id, { state: 'closed' });
    // Closed generations are never delivered. TTL cleanup drains their queues
    // without making a host revocation/closure delete an entire room inbox.
  }
  async function view(s, room, user) {
    const members = (await s.listMembers(room.$id)).filter(m => !m.revoked);
    const peers = await s.listPeers(room.$id);
    const live = peers.filter(p => members.some(m => m.userId === p.hostId && m.sessionId === p.hostSession) &&
      members.some(m => m.userId === p.viewerId && m.sessionId === p.viewerSession));
    const connections = live.map(p => ({ generation: p.generation, viewerId: p.viewerId,
      state: [p.hostId, p.viewerId].every(id => members.some(m => m.userId === id && Date.parse(m.lastSeenAt) + 120000 > now())) ? p.state : 'closed' }));
    const visible = user === room.hostId ? connections : connections.filter(p => p.viewerId === user);
    return { ...safe(room, members, user), connections: visible, connection: visible.length === 1 ? visible[0] : null };
  }
  async function limit(user, kind, max = kind === 'create' ? createLimitPerHour : 30) {
    if (max === 0) return; // Explicit development-only opt-out; public deployments keep a positive cap.
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
  async function publicLimit(kind, entropy, max = kind === 'create' ? createLimitPerHour : 300) {
    if (max === 0) return;
    const shards = Math.min(limits.publicBudgetShards, max);
    const start = parseInt(digest(entropy).slice(0, 8), 16) % shards;
    // Fixed per-shard quotas sum to the exact service budget. Spill to another
    // shard only when full; no cross-room shared write on the normal path.
    for (let offset = 0; offset < shards; ++offset) {
      const shard = (start + offset) % shards;
      const quota = Math.floor(max / shards) + (shard < max % shards ? 1 : 0);
      try { await limit(`public-${kind}:${shard}`, kind, quota); return; }
      catch (e) { if (e.code !== 'RATE_LIMIT') throw e; }
    }
    fail(429, 'RATE_LIMIT', 'Too many room requests');
  }
  async function heartbeatIn(s, id, user, room) {
    await s.updateMember(memberId(id, user), { lastSeenAt: timestamp() });
    if (room.hostId === user) await s.updateRoom(id, { expiresAt: new Date(now() + reconnectWindowMs).toISOString() });
  }
  return {
    authorize, active, inspect, token, timestamp, now, limit, publicLimit, heartbeatIn, view,
    async create(user, input = {}, bootstrap = {}) {
      const title = input.title ?? 'Watch party';
      if (typeof title !== 'string' || !title.trim() || title.trim().length > 80) fail(400, 'INVALID_TITLE', 'Title must be 1 to 80 characters');
      if (bootstrap.budgetIdentity) await publicLimit('create', user);
      else await limit(user,'create');
      const id = bootstrap.roomId || token(), inviteCode = bootstrap.inviteCode || random(24).toString('base64url'), sessionId = token();
      return store.atomic(async s => {
        let existing = await s.getRoom(id);
        if (existing) {
          existing = await lockRoom(s, id);
          active(existing);
          if (existing.hostId !== user) fail(403, 'NOT_A_MEMBER', 'Not a room member');
          if (existing.title !== title.trim()) fail(409, 'REQUEST_REUSE', 'Start request key reused with different input');
          const member = await s.getMember(memberId(id, user));
          if (!member || member.revoked) fail(403, 'CREDENTIAL_REVOKED', 'Membership was revoked');
          await s.updateMember(memberId(id, user), { lastSeenAt: timestamp() });
          existing.expiresAt = new Date(now() + reconnectWindowMs).toISOString();
          await s.updateRoom(id, { expiresAt: existing.expiresAt });
          return { ...safe(existing, await s.listMembers(id), user, member.sessionId), inviteCode };
        }
        const r = await s.createRoom(id, { hostId: user, title: title.trim(), inviteHash: digest(inviteCode), status: 'open', revision: 0,
          expiresAt: new Date(now() + reconnectWindowMs).toISOString() });
        const m = { roomId: id, userId: user, joinedAt: timestamp(), lastSeenAt: timestamp(), sessionId, rateWindow: 0, rateCount: 0 };
        await s.createMember(memberId(id, user), m);
        return { ...safe(r, [m], user, sessionId), inviteCode };
      });
    },
    async join(id, user, invite, renewal = {}) {
      const renewing=renewal.previousSession!==undefined || renewal.requestId!==undefined;
      if(renewing && (typeof renewal.previousSession !== 'string' || typeof renewal.requestId !== 'string' ||
        !idPattern.test(renewal.previousSession) || !idPattern.test(renewal.requestId)))
        fail(400,'INVALID_RENEWAL','Previous session and request identifier required');
      // New guests have no prior identity. Apply the join-attempt budget per room,
      // including failed invitations, instead of letting each attempt mint a new identity.
      // Do not allocate durable per-room counters for attacker-generated nonexistent IDs.
      active(await get(store, id));
      await limit('join-room:'+id,'join');
      return store.atomic(async s => {
        const r = await lockRoom(s, id); active(r);
        const mid = memberId(id, user), existing = await s.getMember(mid);
        if (existing?.revoked) fail(403, 'CREDENTIAL_REVOKED', 'Membership was revoked');
        if (existing && renewal.bootstrapRetry) {
          await s.updateMember(mid, { lastSeenAt: timestamp() });
          return safe(r, await s.listMembers(id), user, existing.sessionId);
        }
        const renewed=renewing?digest(`renew:${id}:${user}:${renewal.previousSession}:${renewal.requestId}`).slice(0,32):null;
        if(renewing) {
          if(!existing)fail(403,'NOT_A_MEMBER','Membership no longer exists');
          if(existing.sessionId===renewed && Date.parse(existing.lastSeenAt)+120000>now()) {
            await s.updateMember(mid,{lastSeenAt:timestamp()});
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
          if (members.length >= limits.memberRecords) fail(409, 'ROOM_MEMBERSHIP_LIMIT', 'Start a new room to admit more participant identities');
          // One host + configurable viewers; stale viewers are explicitly removed before admitting another.
          for (const m of members) if (!m.revoked && m.userId !== r.hostId && Date.parse(m.lastSeenAt) + reconnectWindowMs <= now()) {
            await revoke(s, id, m.userId); await s.updateMember(memberId(id, m.userId), { revoked: 'yes' });
          }
          if (members.filter(m => !m.revoked && (m.userId === r.hostId || Date.parse(m.lastSeenAt) + reconnectWindowMs > now())).length >= maxViewers + 1)
            fail(409, 'ROOM_FULL', 'Watch party is full');
        } else await revoke(s, id, user);
        const sessionId = renewed || token();
        const m = { roomId: id, userId: user, joinedAt: existing?.joinedAt || timestamp(), lastSeenAt: timestamp(), sessionId, rateWindow: 0, rateCount: 0 };
        if (existing) await s.updateMember(mid, m); else await s.createMember(mid, m);
        if (r.hostId === user) {
          r.expiresAt = new Date(now() + reconnectWindowMs).toISOString();
          await s.updateRoom(id, { expiresAt: r.expiresAt });
        }
        return safe(r, await s.listMembers(id), user, sessionId);
      });
    },
    async read(id, user, session) {
      return inspect(id, user, session, (s, room) => view(s, room, user), Boolean(session));
    },
    async sync(id, user, session, messages) {
      // One coherent read/revalidation boundary for authority and inbox; no transaction.
      return inspect(id, user, session, async (s, room) => ({
        room: await view(s, room, user), ...(await messages(s, room, user, session)) }));
    },
    async revokeMember(id, user, session, viewerId) {
      return store.atomic(async s => {
        const { room } = await authorize(s, id, user, session);
        if (room.hostId !== user || viewerId === user) fail(403, 'HOST_ONLY', 'Only host can revoke viewers');
        const mid = memberId(id, viewerId), member = await s.getMember(mid);
        if (!member) fail(404, 'NOT_A_MEMBER', 'Not a room member');
        await revoke(s, id, viewerId);
        await s.updateMember(mid, { revoked: 'yes' });
        return { revoked: true };
      });
    },
    async heartbeat(id, user, session) {
      return store.atomic(async s => {
        const { room } = await authorize(s, id, user, session);
        await heartbeatIn(s, id, user, room);
        return { ok: true, serverTimeMs: now() };
      });
    },
    async leave(id, user, session) {
      return store.atomic(async s => {
        const { room } = await authorize(s, id, user, session, true, true);
        await revoke(s, id, user);
        if (room.hostId === user) await s.updateRoom(id, { status: 'closed', revision: (room.revision || 0) + 1 });
        else await s.updateMember(memberId(id, user), { revoked: 'yes' });
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
      const candidates = async (status, before) => store.listCleanupRooms
        ? store.listCleanupRooms(status, new Date(before).toISOString())
        : (await store.listRooms()).filter(r => r.status === status && Date.parse(r.expiresAt) <= before).slice(0, limits.cleanupRooms);
      for (const r of await candidates('open', now())) {
        await store.atomic(async s => {
          const fresh = await lockRoom(s, r.$id);
          if (fresh.status !== 'open' || Date.parse(fresh.expiresAt) > now()) return;
          // Closing the room invalidates every credential immediately. Queue
          // deletion belongs to bounded GC, not this authorization transaction.
          await s.updateRoom(r.$id, { status: 'closed', revision: (fresh.revision || 0) + 1 });
        });
      }
      // Closed rooms are retained until one day after their original expiry for diagnostics.
      // Drain dependencies in bounded transactions; retain the room fence until
      // the last batch so concurrent cleanup cannot orphan a dependent record.
      for (const r of await candidates('closed', now() - 86400000)) {
        await store.atomic(async s=>{
          const fresh=await lockRoom(s,r.$id);
          if(fresh.status!=='closed' || Date.parse(fresh.expiresAt)+86400000>now())return;
          let remaining = limits.cleanupWrites, pending = false;
          for (const [list, remove] of [[s.listSignals, s.deleteSignal], [s.listPeers, s.deletePeer], [s.listMembers, s.deleteMember]]) {
            const records = await list(r.$id);
            for (const record of records.slice(0, remaining)) await remove(record.$id);
            if (records.length > remaining) pending = true;
            remaining = Math.max(0, remaining - records.length);
          }
          if (pending) return;
          if (s.deleteLimit) {
            const budget = digest('limit:join-room:' + r.$id).slice(0, 32);
            if (await s.getLimit(budget)) await s.deleteLimit(budget);
          }
          await s.deleteRoom(r.$id);
        });
      }
      const expiredSignals = store.listExpiredSignals ? await store.listExpiredSignals(timestamp()) :
        (await Promise.all((await store.listRooms()).map(r => store.listSignals(r.$id)))).flat().filter(m => Date.parse(m.expiresAt) <= now()).slice(0, limits.cleanupExpired);
      for (const m of expiredSignals) { try { await store.deleteSignal(m.$id); } catch (e) { if (e.code !== 404) throw e; } }
      const expiredReceipts = store.listExpiredNotifications ? await store.listExpiredNotifications(timestamp()) :
        (await store.listNotifications()).filter(n => Date.parse(n.expiresAt) <= now()).slice(0, limits.cleanupExpired);
      for (const n of expiredReceipts) {
        try { await store.deleteNotification(n.$id); } catch (e) { if (e.code !== 404) throw e; }
      }
      return { cleaned: true };
    },
  };
}

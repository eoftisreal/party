import test from 'node:test';
import assert from 'node:assert/strict';
import { makeHandler } from '../src/http.js';
import { fixture } from './fixture.js';
import { reconnectWindowMs } from '../src/rooms.js';

test('20-minute outage keeps room, invitation, and viewer membership through cleanup and recovery',async()=>{
  // Verify the legacy one-viewer grace-window behavior using an explicit one-viewer cap.
  const f=fixture({maxViewers:1}),r=await f.rooms.create('host'),v=await f.rooms.join(r.roomId,'viewer',r.inviteCode);
  f.advance(20*60000);await f.rooms.cleanup();
  assert.equal(f.rs.get(r.roomId).status,'open');assert.equal(f.ms.size,2);
  const state=await f.rooms.read(r.roomId,'host');
  assert.ok(state.participants.every(p=>!p.online));
  await assert.rejects(f.rooms.join(r.roomId,'stranger',r.inviteCode),e=>e.code==='ROOM_FULL');
  const host=await f.rooms.join(r.roomId,'host',undefined,{previousSession:r.sessionId,requestId:'a'.repeat(32)});
  const viewer=await f.rooms.join(r.roomId,'viewer',undefined,{previousSession:v.sessionId,requestId:'b'.repeat(32)});
  assert.equal(host.hostId,r.hostId);assert.equal(viewer.yourRole,'viewer');
  assert.equal(host.roomId,r.roomId);
  const peer=await f.signaling.connect(r.roomId,'host',host.sessionId,{viewerId:'viewer'});
  assert.ok(peer.generation);
  assert.equal(Date.parse(f.rs.get(r.roomId).expiresAt),f.rooms.now()+reconnectWindowMs);
});

test('host heartbeat slides grace period; viewer activity cannot extend it',async()=>{
  const f=fixture(),r=await f.rooms.create('host');
  f.advance(60000);await f.rooms.heartbeat(r.roomId,'host',r.sessionId);
  const deadline=Date.parse(f.rs.get(r.roomId).expiresAt);
  assert.equal(deadline,f.rooms.now()+reconnectWindowMs);
  f.advance(20*60000);
  const viewer=await f.rooms.join(r.roomId,'viewer',r.inviteCode);
  await f.rooms.heartbeat(r.roomId,'viewer',viewer.sessionId);
  assert.equal(Date.parse(f.rs.get(r.roomId).expiresAt),deadline);
  f.advance(10*60000);
  await assert.rejects(f.rooms.join(r.roomId,'host'),e=>e.code==='ROOM_CLOSED');
  await f.rooms.cleanup();assert.equal(f.rs.get(r.roomId).status,'closed');
});

test('explicit end works even when host liveness lease expired',async()=>{
  for(const action of ['leave','close']) {
    const f=fixture(),r=await f.rooms.create('host');f.advance(10*60000);
    await assert.rejects(f.rooms[action](r.roomId,'host','f'.repeat(32)),e=>e.code==='STALE_SESSION');
    await f.rooms[action](r.roomId,'host',r.sessionId);
    assert.equal(f.rs.get(r.roomId).status,'closed');
  }
});

test('only viewers absent for the full grace window can be replaced',async()=>{
  const f=fixture(),r=await f.rooms.create('host'),v=await f.rooms.join(r.roomId,'viewer',r.inviteCode);
  for(let i=0;i<31;i++){f.advance(60000);await f.rooms.heartbeat(r.roomId,'host',r.sessionId);}
  const next=await f.rooms.join(r.roomId,'new-viewer',r.inviteCode);
  assert.equal(next.yourRole,'viewer');
  await assert.rejects(f.rooms.join(r.roomId,'viewer',undefined,{previousSession:v.sessionId,requestId:'a'.repeat(32)}),e=>e.code==='CREDENTIAL_REVOKED');
});

test('legacy JWT cannot impersonate a guest by registering its public user ID',async()=>{
  const f=fixture(),handler=makeHandler({service:f.rooms,authenticate:async()=> 'g_'+'a'.repeat(32)});
  let status;
  await handler({req:{method:'POST',path:'/v1/rooms',headers:{authorization:'Bearer legacy-jwt'},bodyJson:{}},res:{json:(_body,s)=>status=s}});
  assert.equal(status,401);assert.equal(f.rs.size,0);
});

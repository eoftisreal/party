import test from 'node:test';
import assert from 'node:assert/strict';
import { makeHandler } from '../src/http.js';
import { fixture } from './fixture.js';
import { reconnectWindowMs } from '../src/rooms.js';

function client(f) {
  const handler = makeHandler({service:f.rooms, signaling:f.signaling, authenticate:async()=>{throw Object.assign(Error('invalid'),{code:401});}});
  return async (method, path, token, input={}, session) => {
    let result;
    await handler({req:{method,path,bodyJson:input,headers:{...(token?{authorization:`Bearer ${token}`} : {}),...(session?{'x-party-session':session}: {})}},res:{json:(body,status)=>result={body,status}}});
    return result;
  };
}

test('guest host and viewer need no Appwrite account, preserve roles, and cannot spoof identities', async()=>{
  const f=fixture(), call=client(f);
  const host=(await call('POST','/v1/guest')).body, viewer=(await call('POST','/v1/guest')).body;
  assert.notEqual(host.guestToken,viewer.guestToken);
  assert.match(host.guestToken,/^guest_[a-f0-9]{64}$/);
  const created=await call('POST','/v1/rooms',host.guestToken,{hostId:viewer.userId});
  assert.equal(created.status,201);assert.equal(created.body.hostId,host.userId);
  const r=created.body,base=`/v1/rooms/${r.roomId}`;
  assert.equal((await call('POST',base+'/join',viewer.guestToken,{inviteCode:'x'.repeat(32)})).status,403);
  const joined=await call('POST',base+'/join',viewer.guestToken,{inviteCode:r.inviteCode});
  assert.equal(joined.status,200);assert.equal(joined.body.yourRole,'viewer');
  assert.equal((await call('POST',base+'/close',viewer.guestToken,{},joined.body.sessionId)).body.error.code,'HOST_ONLY');
  const stranger=(await call('POST','/v1/guest')).body;
  assert.equal((await call('GET',base,stranger.guestToken)).body.error.code,'NOT_A_MEMBER');
  assert.equal((await call('POST',base+'/heartbeat',host.guestToken,{},joined.body.sessionId)).body.error.code,'STALE_SESSION');
  assert.equal(JSON.stringify([...f.rs.values(),...f.ms.values()]).includes(host.guestToken),false);
  assert.equal((await call('POST',base+'/close',host.guestToken,{},r.sessionId)).status,200);
  assert.equal((await call('POST',base+'/join',host.guestToken)).body.error.code,'ROOM_CLOSED');
});

test('room APIs still refuse absent/malformed credentials and guest bootstrap validates input',async()=>{
  const call=client(fixture());
  for(const token of [undefined,'guest_bad','guest_'+'a'.repeat(63),'invalid-jwt'])
    assert.equal((await call('POST','/v1/rooms',token)).status,401);
  assert.equal((await call('POST','/v1/guest',undefined,[])).status,400);
  assert.equal((await call('POST','/v1/guest',undefined,{huge:'x'.repeat(70000)})).status,413);
});

test('20-minute outage keeps room, invitation, and viewer membership through cleanup and recovery',async()=>{
  const f=fixture(),r=await f.rooms.create('host'),v=await f.rooms.join(r.roomId,'viewer',r.inviteCode);
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
  await assert.rejects(f.rooms.join(r.roomId,'viewer',undefined,{previousSession:v.sessionId,requestId:'a'.repeat(32)}),e=>e.code==='NOT_A_MEMBER');
});

test('legacy JWT cannot impersonate a guest by registering its public user ID',async()=>{
  const f=fixture(),handler=makeHandler({service:f.rooms,authenticate:async()=> 'g_'+'a'.repeat(32)});
  let status;
  await handler({req:{method:'POST',path:'/v1/rooms',headers:{authorization:'Bearer legacy-jwt'},bodyJson:{}},res:{json:(_body,s)=>status=s}});
  assert.equal(status,401);assert.equal(f.rs.size,0);
});

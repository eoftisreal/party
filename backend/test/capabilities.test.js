import test from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { makeHandler } from '../src/http.js';
import { fixture } from './fixture.js';
const key = () => randomBytes(32).toString('hex');
function client(f) {
  const handler = makeHandler({service:f.rooms,signaling:f.signaling});
  return async (method,path,input={},credential,session) => {
    let result;
    await handler({req:{method,path,bodyJson:input,headers:{'x-appwrite-user-id':'spoofed-host',
      ...(credential ? {authorization:`Bearer ${credential}`} : {}), ...(session ? {'x-party-session':session} : {})}},
      res:{json:(body,status,headers)=>result={body,status,headers}}});
    assert.equal(result.headers['cache-control'],'no-store');
    return result;
  };
}
async function start(f) {
  const call=client(f),result=await call('POST','/v1/rooms',{requestKey:key()});
  assert.equal(result.status,201);
  const room=result.body,base='/v1/rooms/'+room.roomId;
  return {call,room,base,host:(method,path,input={})=>call(method,base+path,input,room.partyToken,room.sessionId)};
}
test('one-request Start/Join produces distinct room-scoped roles and no persisted bearer secrets',async()=>{
  const f=fixture(),{call,room,base,host}=await start(f);
  assert.match(room.partyToken,/^party_[a-f0-9]{64}$/);
  assert.notEqual(room.hostId,'spoofed-host');
  assert.equal(room.yourRole,'host');assert.equal(room.turnConfigured,false);
  const joined=await call('POST',base+'/join',{inviteCode:room.inviteCode,requestKey:key()});
  assert.equal(joined.status,200);const v=joined.body;
  assert.equal(v.yourRole,'viewer');assert.notEqual(v.partyToken,room.partyToken);
  assert.notEqual(v.yourUserId,room.yourUserId);
  for(const path of ['/close','/peers','/revoke'])
    assert.equal((await call('POST',base+path,{viewerId:room.hostId},v.partyToken,v.sessionId)).status,403);
  for(const path of ['', '/signals','/sync','/ice']) {
    assert.equal((await call('GET',base+path)).status,401);
    assert.equal((await call('GET',base+path,{},room.partyToken)).status,403);
    assert.equal((await call('GET',base+path,{},'party_'+key(),room.sessionId)).status,403);
  }
  const other=await start(f);
  assert.equal((await call('GET',other.base,{},room.partyToken,room.sessionId)).status,403);
  const stored=JSON.stringify(Object.values(f.tables).map(t=>[...t.values()]));
  for(const secret of [room.partyToken,v.partyToken,room.inviteCode])assert.equal(stored.includes(secret),false);
  assert.equal((await host('GET','/ice')).status,200);
});
test('bootstrap response-loss retries are idempotent and revoked credentials cannot be resurrected',async()=>{
  const f=fixture(),call=client(f),requestKey=key();
  const a=(await call('POST','/v1/rooms',{requestKey})).body;
  const b=(await call('POST','/v1/rooms',{requestKey})).body;
  assert.equal(a.partyToken,b.partyToken);assert.equal(a.roomId,b.roomId);assert.equal(a.inviteCode,b.inviteCode);
  assert.equal(f.rs.size,1);
  const path='/v1/rooms/'+a.roomId,join={inviteCode:a.inviteCode,requestKey:key()};
  const v=(await call('POST',path+'/join',join)).body;
  assert.deepEqual((await call('POST',path+'/join',join)).body,v);assert.equal(f.ms.size,2);
  assert.equal((await call('POST',path+'/revoke',{viewerId:v.yourUserId},a.partyToken,a.sessionId)).status,200);
  assert.equal((await call('GET',path+'/sync',{},v.partyToken,v.sessionId)).body.error.code,'CREDENTIAL_REVOKED');
  assert.equal((await call('POST',path+'/join',join)).body.error.code,'CREDENTIAL_REVOKED');
  f.advance(121000);
  assert.equal((await call('POST',path+'/join',{previousSession:v.sessionId,requestId:key().slice(0,32)},v.partyToken)).body.error.code,'CREDENTIAL_REVOKED');
});
test('concurrent invitation joins enforce five-viewer capacity; leaving does not revoke other viewers',async()=>{
  const f=fixture(),{call,room,base,host}=await start(f);
  const results=await Promise.all(Array.from({length:10},()=>call('POST',base+'/join',{inviteCode:room.inviteCode,requestKey:key()})));
  const viewers=results.filter(r=>r.status===200).map(r=>r.body);
  assert.equal(viewers.length,5);assert.equal(results.filter(r=>r.body.error?.code==='ROOM_FULL').length,5);
  for(const v of viewers)assert.equal((await host('POST','/peers',{viewerId:v.yourUserId})).status,200);
  assert.equal((await host('GET','/sync')).body.room.connections.length,5);
  assert.equal((await call('POST',base+'/leave',{},viewers[0].partyToken,viewers[0].sessionId)).status,200);
  for(const v of viewers.slice(1))assert.equal((await call('GET',base+'/sync',{},v.partyToken,v.sessionId)).status,200);
  assert.equal((await host('GET','/sync')).body.room.connections.length,4);
});
test('outage renewal preserves membership, retries safely and rejects expired or stale sessions',async()=>{
  const f=fixture(),{call,room,base}=await start(f),v=(await call('POST',base+'/join',{inviteCode:room.inviteCode})).body;
  f.advance(20*60000);await f.rooms.cleanup();assert.equal(f.rs.get(room.roomId).status,'open');
  assert.equal((await call('GET',base+'/sync',{},room.partyToken,room.sessionId)).body.error.code,'PEER_EXPIRED');
  const renewal={previousSession:room.sessionId,requestId:key().slice(0,32)};
  const h=await call('POST',base+'/join',renewal,room.partyToken);assert.equal(h.status,200);
  assert.equal(h.body.partyToken,room.partyToken);assert.equal(h.body.yourRole,'host');
  assert.equal((await call('POST',base+'/join',renewal,room.partyToken)).body.sessionId,h.body.sessionId);
  assert.equal((await call('GET',base+'/sync',{},room.partyToken,room.sessionId)).body.error.code,'STALE_SESSION');
  const viewer=await call('POST',base+'/join',{previousSession:v.sessionId,requestId:key().slice(0,32)},v.partyToken);
  assert.equal(viewer.status,200);assert.equal(viewer.body.yourUserId,v.yourUserId);
  f.advance(30*60000+1);
  assert.equal((await call('GET',base+'/sync',{},room.partyToken,h.body.sessionId)).status,410);
});
test('public budgets cannot be bypassed with fresh credentials and failed invitations consume budget',async()=>{
  const f=fixture({createLimitPerHour:2}),{call,room,base}=await start(f);
  assert.equal((await call('POST','/v1/rooms')).status,201);
  assert.equal((await call('POST','/v1/rooms')).status,429);
  for(let i=0;i<30;++i)assert.equal((await call('POST',base+'/join',{inviteCode:'x'.repeat(32)})).status,403);
  assert.equal((await call('POST',base+'/join',{inviteCode:room.inviteCode})).status,429);
});
test('legacy auth/bootstrap endpoints, malformed bodies and renewal requests are rejected',async()=>{
  const f=fixture(),{call,room,base}=await start(f);
  for(const token of ['legacy-jwt','guest_'+key()])assert.equal((await call('POST','/v1/rooms',{},token)).status,401);
  assert.equal((await call('POST','/v1/guest')).status,404);
  assert.equal((await call('POST','/v1/rooms',{huge:'x'.repeat(70000)})).status,413);
  assert.equal((await call('POST','/v1/rooms',[])).status,400);
  assert.equal((await call('POST','/v1/rooms',{requestKey:'short'})).status,400);
  assert.equal((await call('POST',base+'/join',{previousSession:room.sessionId,requestId:key().slice(0,32)})).status,401);
  assert.equal((await call('POST',base+'/join',{},room.partyToken)).status,400);
});

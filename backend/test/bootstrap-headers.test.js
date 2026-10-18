import test from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { fixture } from './fixture.js';
import { makeHandler } from '../src/http.js';

test('public start/join tolerate ingress authorization without granting room authority', async () => {
  const f=fixture();
  const handler=makeHandler({service:f.rooms,signaling:f.signaling,getIceSettings:()=>({iceServers:[],ttlSeconds:3600})});
  const request=async(method,path,input={},authorization='Basic fixture-ingress',session)=>{
    let result;
    await handler({req:{method,path,bodyJson:input,headers:{authorization,...(session?{'x-party-session':session}:{})}},
      res:{json:(body,status)=>{result={body,status};}}});
    return result;
  };
  const created=await request('POST','/v1/rooms',{requestKey:randomBytes(32).toString('hex')});
  assert.equal(created.status,201);
  const room=created.body,base='/v1/rooms/'+room.roomId;
  const joined=await request('POST',base+'/join',{inviteCode:room.inviteCode,requestKey:randomBytes(32).toString('hex')});
  assert.equal(joined.status,200);assert.equal(joined.body.yourRole,'viewer');
  assert.notEqual(joined.body.partyToken,room.partyToken);
  assert.equal((await request('POST',base+'/join',{inviteCode:'wrong'})).status,403);
  assert.equal((await request('POST',base+'/join',{previousSession:room.sessionId,requestId:'a'.repeat(32)})).status,401);
  for(const [method,action] of [['GET','/sync'],['GET','/ice'],['POST','/heartbeat'],['POST','/close'],['POST','/peers']])
    assert.equal((await request(method,base+action,{},undefined,room.sessionId)).status,401);
  const headers='Bearer '+joined.body.partyToken;
  assert.equal((await request('POST',base+'/close',{},headers,joined.body.sessionId)).status,403);
  assert.equal((await request('GET',base+'/sync',{},'Bearer '+room.partyToken,room.sessionId)).status,200);
  assert.equal((await request('POST',base+'/close',{},'Bearer '+room.partyToken,room.sessionId)).status,200);
});

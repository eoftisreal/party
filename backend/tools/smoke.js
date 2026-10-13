// Live coordination smoke, not a media/Realtime test. Requires two real user JWTs.
import assert from 'node:assert/strict';
import {randomBytes} from 'node:crypto';
const base=process.env.PARTY_FUNCTION_URL,host=process.env.PARTY_HOST_JWT,viewer=process.env.PARTY_VIEWER_JWT;
if(!base?.startsWith('https://')||!host||!viewer)throw Error('Need HTTPS PARTY_FUNCTION_URL, PARTY_HOST_JWT and PARTY_VIEWER_JWT');
const id=()=>randomBytes(16).toString('hex');
async function call(token,method,path,body,session){
  const response=await fetch(base+path,{method,redirect:'error',signal:AbortSignal.timeout(15000),headers:{Authorization:`Bearer ${token}`,'Content-Type':'application/json',...(session?{'X-Party-Session':session}:{})},...(body?{body:JSON.stringify(body)}:{})});
  return {status:response.status,body:await response.json()};
}
// Syntactically valid negotiation data exercises permissions and ordering only.
// This smoke does not establish DTLS/ICE, and these test fingerprints represent no media peer.
function sdp(direction){return ['v=0','o=- 1 1 IN IP4 127.0.0.1','s=coordination-smoke','t=0 0',
  'a=ice-ufrag:smoke','a=ice-pwd:smoke-test-only-abcdefghijkl',
  'a=fingerprint:sha-256 '+Array(32).fill('AA').join(':'),
  'm=video 9 UDP/TLS/RTP/SAVPF 96','a=mid:video','a='+direction,'a=rtpmap:96 H264/90000',
  'm=audio 9 UDP/TLS/RTP/SAVPF 111','a=mid:audio','a='+direction,'a=rtpmap:111 opus/48000/2',''].join('\r\n');}
const created=await call(host,'POST','/v1/rooms',{title:'Live deployment smoke'});assert.equal(created.status,201);
const r=created.body,path='/v1/rooms/'+r.roomId;
try{
  assert.equal((await call('invalid','GET',path)).status,401);
  assert.equal((await call(viewer,'GET',path)).status,403);
  assert.equal((await call(viewer,'POST',path+'/join',{inviteCode:'x'.repeat(32)})).status,403);
  const joined=await call(viewer,'POST',path+'/join',{inviteCode:r.inviteCode});assert.equal(joined.status,200);
  const v=joined.body;assert.notEqual(v.yourUserId,r.yourUserId,'Smoke test needs two different accounts');
  assert.equal((await call(viewer,'POST',path+'/peers',{viewerId:r.hostId},v.sessionId)).status,403);
  const connect={viewerId:v.yourUserId,requestId:id()},p=await call(host,'POST',path+'/peers',connect,r.sessionId);assert.equal(p.status,200);
  assert.equal((await call(host,'GET',path,undefined,r.sessionId)).body.connection.generation,p.body.generation);
  const offer={messageId:id(),to:v.yourUserId,generation:p.body.generation,type:'offer',payload:{sdp:sdp('sendonly')}};
  const answer={messageId:id(),to:r.yourUserId,generation:p.body.generation,type:'answer',payload:{sdp:sdp('recvonly')}};
  assert.equal((await call(viewer,'POST',path+'/signals',answer,v.sessionId)).body.error.code,'SIGNAL_ORDER');
  assert.equal((await call(host,'POST',path+'/signals',offer,r.sessionId)).status,202);
  const inbox=await call(viewer,'GET',path+'/signals',undefined,v.sessionId);assert.equal(inbox.status,200);
  assert.equal(inbox.body.messages[0].type,'offer');
  assert.equal((await call(viewer,'POST',path+'/ack',{messageIds:inbox.body.messages.map(m=>m.messageId)},v.sessionId)).status,200);
  assert.equal((await call(host,'POST',path+'/signals',offer,r.sessionId)).status,202,'Retry after ACK must be idempotent');
  assert.equal((await call(viewer,'GET',path+'/signals',undefined,v.sessionId)).body.messages.length,0,'ACKed offer replayed');
  assert.equal((await call(viewer,'POST',path+'/signals',answer,v.sessionId)).status,202);
  const ice={messageId:id(),to:v.yourUserId,generation:p.body.generation,type:'candidate',payload:{candidate:'candidate:1 1 UDP 1 192.0.2.1 5000 typ host',mid:'video'}};
  assert.equal((await call(host,'POST',path+'/signals',ice,r.sessionId)).status,202);
  assert.equal((await call(viewer,'GET',path+'/signals',undefined,v.sessionId)).body.messages[0].type,'candidate');
  assert.equal((await call(host,'GET',path+'/signals',undefined,r.sessionId)).body.messages[0].type,'answer');
  const restart={...connect,restart:true,requestId:id()};
  const next=await call(host,'POST',path+'/peers',restart,r.sessionId);assert.equal(next.status,200);
  assert.notEqual(next.body.generation,p.body.generation);
  assert.equal((await call(host,'POST',path+'/peers',restart,r.sessionId)).body.generation,next.body.generation,'Restart retry rotated generation');
  assert.equal((await call(host,'POST',path+'/signals',{...ice,messageId:id()},r.sessionId)).body.error.code,'STALE_GENERATION');
  const rejoined=await call(viewer,'POST',path+'/join',{});assert.equal(rejoined.status,200);
  assert.equal((await call(viewer,'GET',path+'/signals',undefined,v.sessionId)).body.error.code,'STALE_SESSION');
  assert.equal((await call(viewer,'POST',path+'/leave',{},rejoined.body.sessionId)).status,200);
  assert.equal((await call(host,'POST',path+'/peers',{viewerId:v.yourUserId},r.sessionId)).body.error.code,'REMOTE_PEER_EXPIRED');
  assert.equal((await call(host,'POST',path+'/heartbeat',{},r.sessionId)).status,200);
  assert.equal((await call(host,'POST',path+'/close',{},r.sessionId)).status,200);
  assert.equal((await call(viewer,'GET',path+'/signals',undefined,v.sessionId)).status,410);
  console.log('Coordination smoke passed: authentication, invitation, SDP/ICE delivery, post-ACK retry, generation/session replacement, departure and closure. Media, Realtime, real JWT renewal and expiry require external tests.');
}finally{await call(host,'POST',path+'/close',{},r.sessionId);}

// Live coordination smoke, not a media/Realtime test. Uses account-free room capabilities.
import assert from 'node:assert/strict';
import {randomBytes} from 'node:crypto';
const base=process.env.PARTY_FUNCTION_URL;
let host,viewer;
if(!base?.startsWith('https://'))throw Error('Need HTTPS PARTY_FUNCTION_URL');
const id=()=>randomBytes(16).toString('hex');
// Slow Cloud requests can outlast the server's 120-second membership leases.
// Refresh through the existing authenticated heartbeat API; do not weaken the lease.
const leases=[];
let leaseRoomPath;
async function request(token,method,path,body,session){
  // Never log credentials, invitation codes, room identifiers, or request/response bodies.
  const route=path.replace(/^\/v1\/rooms\/[a-f0-9]{32}(?=\/|$)/,'/v1/rooms/:room');
  const start=Date.now();
  console.log(`[smoke] START ${method} ${route}`);
  try {
    const response=await fetch(base+path,{method,redirect:'error',signal:AbortSignal.timeout(15000),headers:{...(token?{Authorization:`Bearer ${token}`}:{}),'Content-Type':'application/json',...(session?{'X-Party-Session':session}:{})},...(body?{body:JSON.stringify(body)}:{})});
    const responseBody=await response.json();
    console.log(`[smoke] HTTP ${response.status} ${method} ${route} in ${Date.now()-start}ms`);
    if(response.status>=400 && /^[A-Z_]{2,48}$/.test(responseBody?.error?.code ?? ''))
      console.log(`[smoke] API_ERROR ${method} ${route}: ${responseBody.error.code}`);
    return {status:response.status,body:responseBody};
  } catch (error) {
    const kind=error?.name==='TimeoutError'||error?.name==='AbortError'?'TIMEOUT':error?.name==='SyntaxError'?'INVALID_JSON':'REQUEST_FAILED';
    console.error(`[smoke] ${kind} ${method} ${route} after ${Date.now()-start}ms`);
    throw error;
  }
}
// Keepalive is serialized with the smoke requests so it cannot race room transactions.
// A 45s threshold leaves time for two potentially slow heartbeats and the next request.
async function call(token,method,path,body,session){
  if(leaseRoomPath) for(const lease of leases){
    if(Date.now()-lease.refreshedAt<45000) continue;
    const pulse=await request(lease.token,'POST',leaseRoomPath+'/heartbeat',{},lease.session);
    assert.equal(pulse.status,200,`Keepalive failed with HTTP ${pulse.status}; inspect API_ERROR above`);
    lease.refreshedAt=Date.now();
  }
  return request(token,method,path,body,session);
}
// Syntactically valid negotiation data exercises permissions and ordering only.
// This smoke does not establish DTLS/ICE, and these test fingerprints represent no media peer.
function sdp(direction){return ['v=0','o=- 1 1 IN IP4 127.0.0.1','s=coordination-smoke','t=0 0',
  'a=ice-ufrag:smoke','a=ice-pwd:smoke-test-only-abcdefghijkl',
  'a=fingerprint:sha-256 '+Array(32).fill('AA').join(':'),
  'm=video 9 UDP/TLS/RTP/SAVPF 96','a=mid:video','a='+direction,'a=rtpmap:96 H264/90000',
  'm=audio 9 UDP/TLS/RTP/SAVPF 111','a=mid:audio','a='+direction,'a=rtpmap:111 opus/48000/2',''].join('\r\n');}
const created=await call(null,'POST','/v1/rooms',{title:'Live deployment smoke',requestKey:id()+id()});
assert.equal(created.status,201,`Room creation returned HTTP ${created.status}; inspect this execution's party_backend_error log for operation/status/errorType. Do not share JWTs, API keys, or request bodies.`);
const r=created.body,path='/v1/rooms/'+r.roomId;host=r.partyToken;
leaseRoomPath=path;
leases.push({token:host,session:r.sessionId,refreshedAt:Date.now()});
let viewerLease;
let closed=false;
try{
  assert.equal((await call('invalid','GET',path)).status,401);
  assert.equal((await call('party_'+'f'.repeat(64),'GET',path,undefined,r.sessionId)).status,403);
  assert.equal((await call(viewer,'POST',path+'/join',{inviteCode:'x'.repeat(32)})).status,403);
  const joined=await call(viewer,'POST',path+'/join',{inviteCode:r.inviteCode});assert.equal(joined.status,200);
  const v=joined.body;viewer=v.partyToken;assert.notEqual(v.yourUserId,r.yourUserId,'Smoke test needs two different identities');
  viewerLease={token:viewer,session:v.sessionId,refreshedAt:Date.now()};
  leases.push(viewerLease);
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
  const renewal=await call(viewer,'POST',path+'/join',{previousSession:v.sessionId,requestId:id()});
  assert.equal(renewal.body.error.code,'LEASE_ACTIVE');
  assert.equal((await call(viewer,'POST',path+'/leave',{},v.sessionId)).status,200);
  leases.splice(leases.indexOf(viewerLease),1);
  assert.equal((await call(host,'POST',path+'/peers',{viewerId:v.yourUserId},r.sessionId)).body.error.code,'REMOTE_PEER_EXPIRED');
  assert.equal((await call(host,'POST',path+'/heartbeat',{},r.sessionId)).status,200);
  assert.equal((await call(host,'POST',path+'/close',{},r.sessionId)).status,200);
  closed=true;
  leases.length=0;
  assert.equal((await call(viewer,'GET',path+'/signals',undefined,v.sessionId)).status,410);
  console.log('Coordination smoke passed: authentication, invitation, SDP/ICE delivery, post-ACK retry, generation/session replacement, departure and closure. Media and live deployment expiry require external tests.');
}finally{
  if(!closed) try {
    const cleanup=await request(host,'POST',path+'/close',{},r.sessionId);
    if(cleanup.status!==200 && cleanup.status!==410)
      console.warn(`[smoke] Cleanup returned HTTP ${cleanup.status}; the room will expire automatically`);
  } catch {
    console.warn('[smoke] Cleanup request failed; the room will expire automatically');
  }
}

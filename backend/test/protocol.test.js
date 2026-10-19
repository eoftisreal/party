import test from 'node:test';
import assert from 'node:assert/strict';
import {randomBytes} from 'node:crypto';
import {fixture} from './fixture.js';
import {makeHandler} from '../src/http.js';
import {contract, protocol, limits} from '../src/protocol.js';
import {memberId, digest} from '../src/rooms.js';
const id = () => randomBytes(16).toString('hex');
const rejected = code => e => e.code === code;
const sdp = direction => 'v=0\r\na=ice-ufrag:test\r\na=ice-pwd:test-password\r\na=fingerprint:sha-256 '+Array(32).fill('AA').join(':')+'\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=rtpmap:96 H264/90000\r\na='+direction+'\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\na='+direction+'\r\n';
async function setup() {
  const f=fixture(),r=await f.rooms.create('host'),v=await f.rooms.join(r.roomId,'viewer',r.inviteCode);
  const p=await f.signaling.connect(r.roomId,'host',r.sessionId,{viewerId:'viewer'});
  const signal=(type='candidate',to='viewer')=>({messageId:id(),generation:p.generation,to,type,payload:type==='candidate'
    ? {candidate:'candidate:1 1 UDP 1 192.0.2.1 5000 typ host',mid:'video'} : {sdp:sdp(type==='offer'?'sendonly':'recvonly')}});
  return {...f,r,v,p,signal,exchange:(who,input)=>f.signaling.exchange(r.roomId,who,who==='host'?r.sessionId:v.sessionId,input)};
}
test('v3 contract is advertised in bootstrap; legacy alias preserves expiry types and unsupported majors mutate nothing',async()=>{
  const f=fixture(),handler=makeHandler({service:f.rooms,signaling:f.signaling});
  async function call(method,path,input={},headers={}) {
    let result;await handler({req:{method,path,bodyJson:input,headers},res:{json:(body,status,responseHeaders)=>result={body,status,headers:responseHeaders}}});return result;
  }
  const incompatible=await call('POST','/v3/rooms',{}, {'X-Party-Protocol':'4'});
  assert.equal(incompatible.status,426);assert.equal(incompatible.body.error.code,'PROTOCOL_VERSION_UNSUPPORTED');
  assert.equal(f.rs.size,0);assert.equal(f.tables.Limit.size,0);
  for (const prefix of contract.compatiblePrefixes) {
    const created=await call('POST',prefix+'/rooms');assert.equal(created.status,201);
    assert.deepEqual(created.body.protocol,protocol);assert.equal(created.headers['x-party-protocol'],'3');
    assert.equal(typeof created.body.expiresAt,'number');assert.equal(created.body.iceExpiresAt,created.body.expiresAt);
    assert.equal(created.body.roomExpiresAt,f.rs.get(created.body.roomId).expiresAt);
    const base=prefix+'/rooms/'+created.body.roomId;
    assert.equal((await call('POST',base+'/sync',{})).status,401);
    const headers={authorization:'Bearer '+created.body.partyToken,'x-party-session':created.body.sessionId,'x-party-protocol':'3'};
    const sync=await call('POST',base+'/sync',{},headers);assert.equal(sync.status,200);
    assert.equal(typeof sync.body.room.expiresAt,'string');assert.deepEqual(sync.body.accepted,[]);
  }
  assert.deepEqual((await call('GET','/v3/protocol')).body,{protocol});
});
test('exchange batches offer, ICE, ACK, answer and heartbeat under one fence per transaction',async()=>{
  const f=await setup();assert.equal(f.p.room.connections[0].generation,f.p.generation);
  let transactions=0;const atomic=f.store.atomic;f.store.atomic=work=>{++transactions;return atomic(work);};
  const offer=f.signal('offer'),candidate=f.signal(),revision=f.rs.get(f.r.roomId).revision;
  const sent=await f.exchange('host',{messages:[offer,candidate],heartbeat:true});
  assert.deepEqual(sent.accepted,[offer.messageId,candidate.messageId]);assert.equal(transactions,1);
  assert.equal(f.rs.get(f.r.roomId).revision,revision+1);
  const inbox=await f.signaling.inbox(f.r.roomId,'viewer',f.v.sessionId);
  const answer=f.signal('answer','host');
  const response=await f.exchange('viewer',{messages:[answer],acknowledgements:inbox.messages.map(m=>m.messageId),heartbeat:true});
  assert.equal(transactions,2);assert.equal(response.room.connection.state,'connected');assert.equal(f.tables.Signal.size,1);
  assert.equal((await f.signaling.inbox(f.r.roomId,'host',f.r.sessionId)).messages[0].type,'answer');
  await f.exchange('host',{});assert.equal(transactions,2,'Empty exchange must remain read-only');
});
test('ambiguous exchange retry remains idempotent after recipient ACK; fingerprint reuse fails',async()=>{
  const f=await setup(),message=f.signal('offer'),input={messages:[message],heartbeat:true};
  await f.exchange('host',input); // Pretend the committed response was lost.
  const inbox=await f.signaling.inbox(f.r.roomId,'viewer',f.v.sessionId);
  await f.exchange('viewer',{acknowledgements:inbox.messages.map(m=>m.messageId)});
  const rate=f.ms.get(memberId(f.r.roomId,'host')).rateCount;
  assert.deepEqual((await f.exchange('host',input)).accepted,[message.messageId]);
  assert.equal(f.tables.Signal.size,0);assert.equal(f.ms.get(memberId(f.r.roomId,'host')).rateCount,rate);
  await assert.rejects(f.exchange('host',{messages:[{...message,payload:{sdp:sdp('sendonly')+'a=extmap:1 urn:test\r\n'}}]}),rejected('MESSAGE_REUSE'));
});
test('invalid batch element rolls back earlier messages and reports safe index; wrong ACK rolls back all writes',async()=>{
  const f=await setup(),offer=f.signal('offer'),bad=f.signal('candidate','stranger');
  const revision=f.rs.get(f.r.roomId).revision;
  await assert.rejects(f.exchange('host',{messages:[offer,bad],heartbeat:true}),e=>e.code==='INVALID_TARGET' && e.messageIndex===1);
  assert.equal(f.tables.Signal.size,0);assert.equal(f.tables.Notification.size,0);assert.equal(f.rs.get(f.r.roomId).revision,revision);
  await f.exchange('host',{messages:[offer]});
  const inbox=await f.signaling.inbox(f.r.roomId,'viewer',f.v.sessionId);
  await assert.rejects(f.exchange('host',{messages:[f.signal()],acknowledgements:[inbox.messages[0].messageId]}),rejected('INVALID_ACK'));
  assert.equal(f.tables.Signal.size,1);
});
test('exchange rejects oversized, duplicate and malformed batches and enforces role, session, lease and revocation',async()=>{
  const f=await setup(),message=f.signal();
  for (const input of [{messages:[message,message]}, {messages:Array(limits.signalBatch+1).fill(message)},
    {acknowledgements:Array(limits.ackBatch+1).fill(id())},{heartbeat:1},{unknown:true},{messages:null}]) {
    // Null optional arrays must not be silently interpreted as an empty batch.
    await assert.rejects(f.exchange('host',input),rejected('INVALID_EXCHANGE'));
  }
  await assert.rejects(f.exchange('viewer',{messages:[f.signal('offer','host')]}),rejected('HOST_ONLY'));
  await assert.rejects(f.signaling.exchange(f.r.roomId,'viewer',id(),{}),rejected('STALE_SESSION'));
  f.advance(121000);await assert.rejects(f.exchange('viewer',{}),rejected('PEER_EXPIRED'));
  await f.rooms.join(f.r.roomId,'host');
  const freshHost=await f.store.getMember(memberId(f.r.roomId,'host'));
  await f.rooms.revokeMember(f.r.roomId,'host',freshHost.sessionId,'viewer');
  await assert.rejects(f.exchange('viewer',{}),rejected('CREDENTIAL_REVOKED'));
});
test('per-member signaling budget counts every new message in a batch and rolls back a partial batch at the cap',async()=>{
  const f=await setup(),mid=memberId(f.r.roomId,'host');
  await f.store.updateMember(mid,{rateWindow:Math.floor(f.rooms.now()/60000),rateCount:179});
  await assert.rejects(f.exchange('host',{messages:[f.signal(),f.signal()]}),rejected('RATE_LIMIT'));
  assert.equal(f.ms.get(mid).rateCount,179);assert.equal(f.tables.Signal.size,0);
  await f.exchange('host',{messages:[f.signal()]});assert.equal(f.ms.get(mid).rateCount,180);
  await assert.rejects(f.exchange('host',{messages:[f.signal()]}),rejected('RATE_LIMIT'));
});
test('closed generation queues do not starve a replacement viewer before TTL cleanup',async()=>{
  const f=await setup();
  for(let i=0;i<256;++i)await f.store.createSignal(id(),{roomId:f.r.roomId,generation:f.p.generation,to:'viewer',expiresAt:new Date(f.rooms.now()+120000).toISOString()});
  await f.rooms.revokeMember(f.r.roomId,'host',f.r.sessionId,'viewer');
  const next=await f.rooms.join(f.r.roomId,'replacement',f.r.inviteCode);
  const peer=await f.signaling.connect(f.r.roomId,'host',f.r.sessionId,{viewerId:'replacement'});
  await f.exchange('host',{messages:[{...f.signal('offer','replacement'),generation:peer.generation}]});
  assert.equal((await f.signaling.inbox(f.r.roomId,'replacement',next.sessionId)).messages.length,1);
  await assert.rejects(f.exchange('viewer',{}),rejected('CREDENTIAL_REVOKED'));
});
test('sharded public budgets keep an exact service-wide ceiling with fixed storage and refill',async()=>{
  const f=fixture(),maximum=19;
  const results=await Promise.allSettled(Array.from({length:30},(_,i)=>f.rooms.publicLimit('create','identity-'+i,maximum)));
  assert.equal(results.filter(x=>x.status==='fulfilled').length,maximum);
  assert.ok(results.filter(x=>x.status==='rejected').every(x=>x.reason.code==='RATE_LIMIT'));
  assert.equal(f.tables.Limit.size,limits.publicBudgetShards);
  for(const key of f.tables.Limit.keys())assert.ok(Array.from({length:limits.publicBudgetShards},(_,i)=>digest('limit:public-create:'+i).slice(0,32)).includes(key));
  f.advance(3600000);await f.rooms.publicLimit('create','another',maximum);assert.equal(f.tables.Limit.size,limits.publicBudgetShards);
});
test('cleanup drains retained room records in bounded batches and removes its admission counter only at completion',async()=>{
  const f=fixture(),room=await f.rooms.create('host');
  await f.rooms.join(room.roomId,'viewer',room.inviteCode);
  await f.rooms.close(room.roomId,'host',room.sessionId);
  for(let i=0;i<120;++i)await f.store.createMember('old-'+i,{roomId:room.roomId,revoked:'yes'});
  f.advance(2*86400000);
  let largest=0;const atomic=f.store.atomic;
  f.store.atomic=work=>atomic(async s=>{
    let writes=0;for(const key of Object.keys(s))if(/^(delete|update|bump)/.test(key)){const original=s[key];s[key]=async(...args)=>{++writes;return original(...args);};}
    const result=await work(s);largest=Math.max(largest,writes);return result;
  });
  await f.rooms.cleanup();assert.equal(f.rs.size,1);assert.equal(f.ms.size,74);
  await f.rooms.cleanup();assert.equal(f.rs.size,1);
  await f.rooms.cleanup();assert.equal(f.rs.size,0);assert.equal(f.ms.size,0);
  assert.ok(largest<=limits.cleanupWrites+3);assert.ok(!f.tables.Limit.has(digest('limit:join-room:'+room.roomId).slice(0,32)));
});
test('room identity tombstone cap bounds repeated admission without reviving revoked credentials',async()=>{
  const f=fixture(),room=await f.rooms.create('host');
  for(let i=1;i<limits.memberRecords;++i)await f.store.createMember('old-'+i,{roomId:room.roomId,userId:'old-'+i,revoked:'yes'});
  await assert.rejects(f.rooms.join(room.roomId,'new-viewer',room.inviteCode),rejected('ROOM_MEMBERSHIP_LIMIT'));
  assert.equal((await f.rooms.read(room.roomId,'host',room.sessionId)).participants.length,1);
});
test('cleanup tolerates another scheduled worker removing a candidate before its transaction',async()=>{
  const f=fixture(),first=await f.rooms.create('host-one'),second=await f.rooms.create('host-two');
  f.advance(1800001);
  const atomic=f.store.atomic;let raced=false;
  f.store.atomic=work=>{
    if(!raced){raced=true;f.rs.delete(first.roomId);f.ms.delete(memberId(first.roomId,'host-one'));}
    return atomic(work);
  };
  await f.rooms.cleanup();assert.equal(f.rs.get(second.roomId).status,'closed');
});

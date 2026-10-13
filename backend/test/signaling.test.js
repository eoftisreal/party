import test from 'node:test';
import assert from 'node:assert/strict';
import {fixture} from './fixture.js';
import {ApiError,memberId} from '../src/rooms.js';
import {makeHandler} from '../src/http.js';
const rejects=code=>e=>e instanceof ApiError&&e.code===code;
const sdp=direction=>'v=0\r\na=ice-ufrag:test\r\na=ice-pwd:test-password\r\na=fingerprint:sha-256 '+Array(32).fill('AA').join(':')+'\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=rtpmap:96 H264/90000\r\na='+direction+'\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\na='+direction+'\r\n';
async function connected() {
  const f=fixture(),r=await f.rooms.create('host'),v=await f.rooms.join(r.roomId,'viewer',r.inviteCode);
  const p=await f.signaling.connect(r.roomId,'host',r.sessionId,{viewerId:'viewer'});
  let n=0;
  const send=(who,type,payload,generation=p.generation,to=who==='host'?'viewer':'host')=>f.signaling.send(r.roomId,who,who==='host'?r.sessionId:v.sessionId,{messageId:(++n).toString(16).padStart(32,'0'),to,generation,type,payload});
  return {...f,r,v,p,send};
}
test('host offer, viewer answer, trickled ICE and acknowledgements',async()=>{
  const f=await connected();
  await f.send('host','offer',{sdp:sdp('sendonly')});
  await f.send('host','candidate',{candidate:'candidate:1 1 UDP 2122260223 192.0.2.1 50000 typ host',mid:'video'});
  const inbox=await f.signaling.inbox(f.r.roomId,'viewer',f.v.sessionId);
  assert.deepEqual(inbox.messages.map(x=>x.type),['offer','candidate']);
  await f.send('viewer','answer',{sdp:sdp('recvonly')});
  assert.equal((await f.signaling.inbox(f.r.roomId,'host',f.r.sessionId)).messages.length,1);
  await f.signaling.ack(f.r.roomId,'viewer',f.v.sessionId,{messageIds:inbox.messages.map(m=>m.messageId)});
  assert.equal((await f.signaling.inbox(f.r.roomId,'viewer',f.v.sessionId)).messages.length,0);
});
test('unauthorized stream offers, targets, acknowledgements and stale generations rejected',async()=>{
  const f=await connected();
  await assert.rejects(f.send('viewer','offer',{sdp:sdp('sendonly')}),rejects('HOST_ONLY'));
  await assert.rejects(f.send('host','offer',{sdp:sdp('sendonly')},f.p.generation,'stranger'),rejects('INVALID_TARGET'));
  await assert.rejects(f.signaling.connect(f.r.roomId,'viewer',f.v.sessionId,{viewerId:'host'}),rejects('HOST_ONLY'));
  await f.send('host','offer',{sdp:sdp('sendonly')});
  const message=(await f.signaling.inbox(f.r.roomId,'viewer',f.v.sessionId)).messages[0];
  await assert.rejects(f.signaling.ack(f.r.roomId,'host',f.r.sessionId,{messageIds:[message.messageId]}),rejects('INVALID_ACK'));
  await f.signaling.connect(f.r.roomId,'host',f.r.sessionId,{viewerId:'viewer',restart:true});
  await assert.rejects(f.send('host','offer',{sdp:sdp('sendonly')}),rejects('STALE_GENERATION'));
});
test('rejoin rotates session and revokes queued SDP; leases require reconnect',async()=>{
  const f=await connected(); await f.send('host','offer',{sdp:sdp('sendonly')});
  const again=await f.rooms.join(f.r.roomId,'viewer'); assert.notEqual(again.sessionId,f.v.sessionId);
  await assert.rejects(f.signaling.inbox(f.r.roomId,'viewer',f.v.sessionId),rejects('STALE_SESSION'));
  assert.equal((await f.signaling.inbox(f.r.roomId,'viewer',again.sessionId)).messages.length,0);
  f.advance(120001);
  await assert.rejects(f.rooms.heartbeat(f.r.roomId,'viewer',again.sessionId),rejects('PEER_EXPIRED'));
});
test('closed/expired room cannot signal, and disconnect is delivered once',async()=>{
  const f=await connected(); await f.send('host','offer',{sdp:sdp('sendonly')}); await f.send('viewer','answer',{sdp:sdp('recvonly')});
  await f.send('host','disconnect',{});
  assert.equal((await f.signaling.inbox(f.r.roomId,'viewer',f.v.sessionId)).messages.at(-1).type,'disconnect');
  await f.rooms.close(f.r.roomId,'host',f.r.sessionId);
  await assert.rejects(f.signaling.inbox(f.r.roomId,'viewer',f.v.sessionId),rejects('ROOM_CLOSED'));
  const other=await connected(); other.advance(86400001);
  await assert.rejects(other.signaling.connect(other.r.roomId,'host',other.r.sessionId,{viewerId:'viewer'}),rejects('ROOM_CLOSED'));
});
test('strict schemas reject unencrypted SDP, wrong direction, malformed ICE and unknown messages',async()=>{
  const f=await connected();
  for(const payload of [{sdp:'v=0\r\n'},{sdp:sdp('recvonly')},{sdp:sdp('sendonly').replace('UDP/TLS/RTP/SAVPF','RTP/AVP')},{sdp:sdp('sendonly'),extra:1}])
    await assert.rejects(f.send('host','offer',payload),e=>e.status===400);
  await assert.rejects(f.send('host','candidate',{candidate:'candidate:foo\r\nmalicious',mid:'video'}),rejects('INVALID_ICE'));
  await assert.rejects(f.send('host','movie-file',{url:'movie.mp4'}),rejects('INVALID_SIGNAL'));
});
test('serialized concurrent joins enforce capacity; failed transaction rolls back',async()=>{
  const f=fixture(),r=await f.rooms.create('host');
  const results=await Promise.allSettled(['one','two'].map(user=>f.rooms.join(r.roomId,user,r.inviteCode)));
  assert.equal(results.filter(x=>x.status==='fulfilled').length,1);
  assert.equal(f.ms.size,2);
  await assert.rejects(f.store.atomic(async s=>{await s.updateRoom(r.roomId,{status:'closed'});throw Error('abort');}));
  assert.equal(f.rs.get(r.roomId).status,'open');
});
test('signaling rate limit persists across service instances and room close wins racing sends',async()=>{
  const f=await connected(); await f.send('host','offer',{sdp:sdp('sendonly')});
  const m=f.ms.get(memberId(f.r.roomId,'host'));m.rateCount=180;
  await assert.rejects(f.send('host','candidate',{candidate:'candidate:1 1 UDP 1 192.0.2.1 5000 typ host',mid:'video'}),rejects('RATE_LIMIT'));
  await f.rooms.close(f.r.roomId,'host',f.r.sessionId);
  await assert.rejects(f.send('host','candidate',{candidate:'candidate:1 1 UDP 1 192.0.2.1 5000 typ host',mid:'video'}),rejects('ROOM_CLOSED'));
});
test('HTTP signaling rejects identity spoofing, missing membership session, and oversized parsed bodies',async()=>{
  const f=await connected(),handler=makeHandler({service:f.rooms,signaling:f.signaling,authenticate:async()=> 'viewer'});
  async function call(req){let result;await handler({req:{headers:{authorization:'Bearer jwt'},method:'GET',...req},res:{json:(body,status)=>result={body,status}}});return result;}
  assert.equal((await call({path:`/v1/rooms/${f.r.roomId}/signals`,headers:{authorization:'Bearer jwt','x-appwrite-user-id':'host'}})).status,403);
  assert.equal((await call({path:`/v1/rooms/${f.r.roomId}/signals`,method:'POST',bodyJson:{sdp:'x'.repeat(70000)}})).status,413);
});
test('post-ACK response loss retries are idempotent and conflicting IDs rejected',async()=>{
  const f=await connected();
  const input={messageId:'e'.repeat(32),to:'viewer',generation:f.p.generation,type:'offer',payload:{sdp:sdp('sendonly')}};
  const first=await f.signaling.send(f.r.roomId,'host',f.r.sessionId,input);
  await f.signaling.ack(f.r.roomId,'viewer',f.v.sessionId,{messageIds:[first.messageId]});
  assert.deepEqual(await f.signaling.send(f.r.roomId,'host',f.r.sessionId,input),first);
  assert.equal((await f.signaling.inbox(f.r.roomId,'viewer',f.v.sessionId)).messages.length,0);
  await assert.rejects(f.signaling.send(f.r.roomId,'host',f.r.sessionId,{...input,payload:{sdp:sdp('sendonly')+'a=test:changed\r\n'}}),rejects('MESSAGE_REUSE'));
});
test('restart authorization response loss does not rotate the connection twice',async()=>{
  const f=await connected(),input={viewerId:'viewer',restart:true,requestId:'f'.repeat(32)};
  const first=await f.signaling.connect(f.r.roomId,'host',f.r.sessionId,input);
  const retry=await f.signaling.connect(f.r.roomId,'host',f.r.sessionId,input);
  assert.equal(first.generation,retry.generation);
  assert.equal((await f.rooms.read(f.r.roomId,'host',f.r.sessionId)).connection.generation,first.generation);
});
test('persisted creation and failed invite budgets reject API abuse',async()=>{
  const f=fixture();for(let i=0;i<4;++i)await f.rooms.create('host');
  await assert.rejects(f.rooms.create('host'),rejects('RATE_LIMIT'));
  f.advance(3600000);const r=await f.rooms.create('host');
  for(let i=0;i<30;++i)await assert.rejects(f.rooms.join(r.roomId,'attacker','x'.repeat(32)),rejects('INVALID_INVITE'));
  await assert.rejects(f.rooms.join(r.roomId,'attacker',r.inviteCode),rejects('RATE_LIMIT'));
});

test('queued authorization after viewer departure does not revoke host membership',async()=>{
  const f=await connected();await f.rooms.leave(f.r.roomId,'viewer',f.v.sessionId);
  await assert.rejects(f.signaling.connect(f.r.roomId,'host',f.r.sessionId,{viewerId:'viewer'}),rejects('REMOTE_PEER_EXPIRED'));
  assert.equal((await f.rooms.heartbeat(f.r.roomId,'host',f.r.sessionId)).ok,true);
  assert.equal((await f.rooms.read(f.r.roomId,'host',f.r.sessionId)).status,'open');
});

test('cleanup expires receipts and removes retained closed room dependencies',async()=>{
  const f=await connected();await f.send('host','offer',{sdp:sdp('sendonly')});
  await f.rooms.close(f.r.roomId,'host',f.r.sessionId);f.advance(120001);await f.rooms.cleanup();
  assert.equal(f.tables.Notification.size,0);assert.equal(f.tables.Room.size,1);
  f.advance(2*86400000);await f.rooms.cleanup();
  for(const name of ['Room','Member','Peer','Signal','Notification'])assert.equal(f.tables[name].size,0,name);
});

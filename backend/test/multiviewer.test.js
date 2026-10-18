import test from 'node:test';
import assert from 'node:assert/strict';
import { fixture } from './fixture.js';

const sdp = dir => 'v=0\r\na=ice-ufrag:abc\r\na=ice-pwd:password\r\na=fingerprint:sha-256 ' +
  Array(32).fill('AA').join(':') + '\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=rtpmap:96 H264/90000\r\na=' +
  dir + '\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\na=' + dir + '\r\n';

test('five viewers independently join, negotiate, and retain private per-viewer generations', async () => {
  const f = fixture(), host = await f.rooms.create('host');
  const viewers = [];
  for (let i = 0; i < 5; ++i) {
    const id = 'viewer' + i;
    const joined = await f.rooms.join(host.roomId, id, host.inviteCode);
    const peer = await f.signaling.connect(host.roomId, 'host', host.sessionId, {viewerId:id});
    viewers.push({id, session: joined.sessionId, generation: peer.generation});
  }
  await assert.rejects(f.rooms.join(host.roomId,'viewer6',host.inviteCode),
    e => e.code === 'ROOM_FULL');
  const room = await f.rooms.read(host.roomId,'host',host.sessionId);
  assert.equal(room.maxViewers, 5);
  assert.equal(room.participants.length,6);
  assert.equal(room.connections.length,5);
  assert.equal(new Set(room.connections.map(x=>x.generation)).size,5);
  for(let i=0;i<5;++i){
    const v=viewers[i];
    const single=await f.rooms.read(host.roomId,v.id,v.session);
    assert.equal(single.connections.length,1);
    assert.equal(single.connection.viewerId,v.id);
    await f.signaling.send(host.roomId,'host',host.sessionId,{
      to:v.id,generation:v.generation,messageId:(i+1).toString(16).padStart(32,'0'),
      type:'offer',payload:{sdp:sdp('sendonly')}
    });
    assert.equal((await f.signaling.inbox(host.roomId,v.id,v.session)).messages.length,1);
  }
  assert.equal((await f.signaling.inbox(host.roomId,'host',host.sessionId)).messages.length,0);
  // A single viewer leaving invalidates only its own peer.
  await f.rooms.leave(host.roomId,viewers[1].id,viewers[1].session);
  const after=await f.rooms.read(host.roomId,'host',host.sessionId);
  assert.equal(after.connections.some(c=>c.viewerId===viewers[1].id),false);
  assert.equal(after.connections.filter(c=>c.state!=='closed').length,4);
  const unaffected=viewers[2];
  await f.signaling.send(host.roomId,unaffected.id,unaffected.session,{
    to:'host',generation:unaffected.generation,messageId:'f'.repeat(32),type:'answer',payload:{sdp:sdp('recvonly')}
  });
  assert.equal((await f.signaling.inbox(host.roomId,'host',host.sessionId)).messages.length,1);
  await assert.rejects(f.rooms.join(host.roomId,viewers[1].id,host.inviteCode),e=>e.code==='CREDENTIAL_REVOKED');
  const replacement='replacement-viewer';
  const rejoined=await f.rooms.join(host.roomId,replacement,host.inviteCode);
  assert.equal(rejoined.participants.length,6);
  const restarted=await f.signaling.connect(host.roomId,'host',host.sessionId,{viewerId:replacement,restart:true});
  assert.notEqual(restarted.generation,viewers[1].generation);
  const still=await f.rooms.read(host.roomId,unaffected.id,unaffected.session);
  assert.equal(still.connection.generation,unaffected.generation);
});

test('configurable lower capacity is enforced and invalid limits fail fast', async () => {
  const f=fixture({maxViewers:2}),host=await f.rooms.create('host');
  for(const viewer of ['a','b']) await f.rooms.join(host.roomId,viewer,host.inviteCode);
  await assert.rejects(f.rooms.join(host.roomId,'c',host.inviteCode),e=>e.code==='ROOM_FULL');
  for(const maxViewers of [0,-1,17,1.5,NaN]) assert.throws(()=>fixture({maxViewers}),/maxViewers/);
});

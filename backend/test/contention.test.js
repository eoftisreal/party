import test from 'node:test';
import assert from 'node:assert/strict';
import {fixture} from './fixture.js';

async function connected() {
  const f=fixture(),r=await f.rooms.create('host'),v=await f.rooms.join(r.roomId,'viewer',r.inviteCode);
  const p=await f.signaling.connect(r.roomId,'host',r.sessionId,{viewerId:'viewer'});
  return {...f,r,v,p};
}
const rejected=code=>e=>e.code===code;

test('simultaneous host/viewer room and inbox polls perform zero database writes or commits',async()=>{
  const f=await connected();
  const original=f.store.atomic;
  let writes=0;
  f.store.atomic=async work=>{++writes;return original(work);};
  const revision=f.rs.get(f.r.roomId).revision;
  await Promise.all(Array.from({length:40},(_,i)=>{
    const user=i%2?'viewer':'host',session=i%2?f.v.sessionId:f.r.sessionId;
    return i%4<2?f.rooms.read(f.r.roomId,user,session):f.signaling.inbox(f.r.roomId,user,session);
  }));
  assert.equal(writes,0);assert.equal(f.rs.get(f.r.roomId).revision,revision);
});

test('room close racing an inbox poll rejects the response instead of leaking messages',async()=>{
  const f=await connected(),list=f.store.listSignals;
  let closed=false;
  f.store.listSignals=async id=>{
    const messages=await list(id);
    if(!closed){closed=true;await f.rooms.close(id,'host',f.r.sessionId);}
    return messages;
  };
  await assert.rejects(f.signaling.inbox(f.r.roomId,'viewer',f.v.sessionId),rejected('ROOM_CLOSED'));
});

test('membership replacement racing a poll rejects the old membership session',async()=>{
  const f=await connected(),list=f.store.listSignals;
  let replaced=false;
  f.store.listSignals=async id=>{
    const messages=await list(id);
    if(!replaced){replaced=true;await f.rooms.join(id,'viewer');}
    return messages;
  };
  await assert.rejects(f.signaling.inbox(f.r.roomId,'viewer',f.v.sessionId),rejected('STALE_SESSION'));
});

test('generation change racing a snapshot retries and returns the latest connection',async()=>{
  const f=await connected(),list=f.store.listPeers;
  let next;
  f.store.listPeers=async id=>{
    const peers=await list(id);
    if(!next)next=await f.signaling.connect(id,'host',f.r.sessionId,{viewerId:'viewer',restart:true});
    return peers;
  };
  const read=await f.rooms.read(f.r.roomId,'host',f.r.sessionId);
  assert.notEqual(read.connection.generation,f.p.generation);
  assert.equal(read.connection.generation,next.generation);
});

test('continuous changes exhaust a bounded read retry instead of looping forever',async()=>{
  const f=await connected(),list=f.store.listPeers;
  let attempts=0;
  f.store.listPeers=async id=>{
    const peers=await list(id);++attempts;
    await f.rooms.heartbeat(id,'host',f.r.sessionId);
    return peers;
  };
  await assert.rejects(f.rooms.read(f.r.roomId,'host',f.r.sessionId),rejected('RETRY'));
  assert.equal(attempts,4);
});

test('a viewer leaving while a room snapshot is collected revokes membership',async()=>{
  const f=await connected(),list=f.store.listPeers;
  let left=false;
  f.store.listPeers=async id=>{
    const peers=await list(id);
    if(!left){left=true;await f.rooms.leave(id,'viewer',f.v.sessionId);}
    return peers;
  };
  await assert.rejects(f.rooms.read(f.r.roomId,'viewer',f.v.sessionId),rejected('NOT_A_MEMBER'));
});

test('cleanup rechecks expiry after a racing host heartbeat',async()=>{
  const f=fixture(),r=await f.rooms.create('host');f.advance(30*60000);
  const list=f.store.listRooms;
  let revived=false;
  f.store.listRooms=async()=>{
    const rooms=await list();
    if(!revived){
      revived=true;
      // A heartbeat that won before cleanup stages its room write.
      f.rs.get(r.roomId).expiresAt=new Date(f.rooms.now()+30*60000).toISOString();
      ++f.rs.get(r.roomId).revision;
    }
    return rooms;
  };
  await f.rooms.cleanup();assert.equal(f.rs.get(r.roomId).status,'open');
});

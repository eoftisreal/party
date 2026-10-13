import test from 'node:test';
import assert from 'node:assert/strict';
import {makeRoomsService,ApiError} from '../src/rooms.js';
import {makeHandler} from '../src/http.js';
function fixture(){
 const rs=new Map(),ms=new Map();
 const store={
 createRoom:async(id,data)=>{const r={$id:id,...data};rs.set(id,r);return r;},
 getRoom:async id=>rs.get(id)||null,
 updateRoom:async(id,data)=>{rs.set(id,{...rs.get(id),...data});},
 deleteRoom:async id=>{rs.delete(id);},
 createMember:async(id,data)=>{if(ms.has(id))throw Object.assign(Error('duplicate'),{code:409});ms.set(id,data);},
 getMember:async id=>ms.get(id)||null,
 listMembers:async id=>[...ms.values()].filter(x=>x.roomId===id),
 deleteMember:async id=>{ms.delete(id);}
 };
 return {store,rs,ms,rooms:makeRoomsService(store,{now:()=>Date.parse('2026-10-13T12:00:00Z')})};
}
const bad=(status,code)=>e=>e instanceof ApiError&&e.status===status&&e.code===code;
test('create generates 32 char room id, invite, and hides invite hash',async()=>{const {rooms,rs}=fixture();const r=await rooms.create('host',{title:' Party '});assert.match(r.roomId,/^[a-f0-9]{32}$/);assert.match(r.inviteCode,/^[A-Za-z0-9_-]{32}$/);assert.equal(r.title,'Party');assert.equal(r.inviteHash,undefined);assert.equal(rs.get(r.roomId).inviteHash.length,64);});
test('invitation and membership controls',async()=>{const {rooms}=fixture();const r=await rooms.create('host');await assert.rejects(rooms.read(r.roomId,'stranger'),bad(403,'NOT_A_MEMBER'));await assert.rejects(rooms.join(r.roomId,'guest','x'.repeat(32)),bad(403,'INVALID_INVITE'));assert.equal((await rooms.join(r.roomId,'guest',r.inviteCode)).participants.length,2);assert.equal((await rooms.join(r.roomId,'guest')).participants.length,2);await rooms.leave(r.roomId,'guest');await assert.rejects(rooms.read(r.roomId,'guest'),bad(403,'NOT_A_MEMBER'));});
test('host-only close and expiry',async()=>{const {rooms}=fixture();const r=await rooms.create('host');await rooms.join(r.roomId,'guest',r.inviteCode);await assert.rejects(rooms.close(r.roomId,'guest'),bad(403,'HOST_ONLY'));await rooms.close(r.roomId,'host');await assert.rejects(rooms.join(r.roomId,'new',r.inviteCode),bad(410,'ROOM_CLOSED'));});
test('invalid room title and ids',async()=>{const {rooms}=fixture();await assert.rejects(rooms.create('host',{title:''}),bad(400,'INVALID_TITLE'));await assert.rejects(rooms.read('invalid','host'),bad(400,'INVALID_ROOM_ID'));});
test('HTTP refuses missing or invalid JWT and ignores spoofed identity',async()=>{const {rooms}=fixture();const handler=makeHandler({service:rooms,authenticate:async t=>{if(t!=='valid.jwt')throw Object.assign(Error('Unauthorized'),{code:401});return 'real-user';}});async function call(headers={}){let output;await handler({req:{path:'/v1/rooms',method:'POST',headers,bodyJson:{title:'Party'}},res:{json:(v,s)=>{output={v,s};}}});return output;}assert.equal((await call()).s,401);assert.equal((await call({authorization:'Bearer bad','x-appwrite-user-id':'spoof'})).s,401);const ok=await call({authorization:'Bearer valid.jwt','x-appwrite-user-id':'spoof'});assert.equal(ok.s,201);assert.equal(ok.v.hostId,'real-user');});

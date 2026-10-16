import test from 'node:test';
import assert from 'node:assert/strict';
import {setTimeout as wait} from 'node:timers/promises';
import {Client,AppwriteException} from 'node-appwrite';
import {makeAppwriteStore} from '../src/store.js';
import {makeRoomsService,memberId} from '../src/rooms.js';
import {makeSignalingService} from '../src/signaling.js';

// Unlike the serialized room fixture, this models Appwrite's optimistic staging:
// reads are live, writes capture versions when staged, and conflicting commits fail.
test('two SDK-backed clients can poll while concurrent heartbeats conflict and recover',async()=>{
  const original=Client.prototype.call,documents=new Map(),versions=new Map(),transactions=new Map();
  const room='a'.repeat(32),hostSession='b'.repeat(32),viewerSession='c'.repeat(32);
  const now=Date.parse('2026-10-16T00:00:00Z');
  let serial=0,commits=0,conflicts=0,revisionStages=0;
  function put(key,value){documents.set(key,value);versions.set(key,(versions.get(key)||0)+1);}
  put('rooms:'+room,{$id:room,hostId:'host',title:'Movie',status:'open',revision:0,expiresAt:new Date(now+1800000).toISOString()});
  for(const [user,sessionId] of [['host',hostSession],['viewer',viewerSession]]) {
    const mid=memberId(room,user);
    put('members:'+mid,{$id:mid,roomId:room,userId:user,sessionId,joinedAt:new Date(now).toISOString(),lastSeenAt:new Date(now).toISOString()});
  }
  Client.prototype.call=async function(method,url,headers,payload){
    const path=url.pathname;
    if(path==='/v1/databases/transactions') {
      const id='tx'+(++serial);transactions.set(id,{staged:new Map(),base:new Map()});return {$id:id};
    }
    if(path.startsWith('/v1/databases/transactions/')) {
      const id=path.split('/').at(-1),tx=transactions.get(id);
      if(payload.rollback){transactions.delete(id);return {};}
      await wait(5); // Give independent Functions overlapping staging windows.
      for(const [key,version] of tx.base)if(versions.get(key)!==version){
        ++conflicts;throw new AppwriteException('conflict',409,'transaction_conflict');
      }
      for(const [key,value] of tx.staged)put(key,structuredClone(value));
      transactions.delete(id);++commits;return {};
    }
    const match=/^\/v1\/databases\/party\/collections\/(\w+)\/documents(?:\/([^/]+))?(\/revision\/increment)?$/.exec(path);
    assert.ok(match,'Unexpected SDK operation: '+path);
    const [,collection,id,increment]=match,tx=transactions.get(payload.transactionId);
    const key=collection+':'+id;
    const get=key=>structuredClone(tx?.staged.has(key)?tx.staged.get(key):documents.get(key));
    if(method==='get') {
      await wait(1);
      if(id){const value=get(key);if(!value)throw new AppwriteException('missing',404,'document_not_found');return value;}
      let values=[...documents].filter(([k])=>k.startsWith(collection+':')).map(([k])=>get(k));
      for(const q of payload.queries||[]) {
        const query=JSON.parse(q);
        if(query.method==='equal')values=values.filter(v=>query.values.includes(v[query.attribute]));
      }
      return {documents:values,total:values.length};
    }
    assert.ok(tx,'No untransactional mutation is allowed');
    if(!tx.base.has(key))tx.base.set(key,versions.get(key));
    const value=get(key);
    if(increment){++revisionStages;value.revision+=payload.value;}
    else Object.assign(value,payload.data);
    tx.staged.set(key,value);return structuredClone(value);
  };
  try {
    const env={APPWRITE_ENDPOINT:'https://example.test/v1',APPWRITE_PROJECT_ID:'project',PARTY_DATABASE_ID:'party'};
    const hostStore=makeAppwriteStore(env,'key',{random:()=>0.2}),viewerStore=makeAppwriteStore(env,'key',{random:()=>0.8});
    const host=makeRoomsService(hostStore,{now:()=>now}),viewer=makeRoomsService(viewerStore,{now:()=>now});
    const hostSignals=makeSignalingService(hostStore,host),viewerSignals=makeSignalingService(viewerStore,viewer);
    await Promise.all([
      host.heartbeat(room,'host',hostSession),viewer.heartbeat(room,'viewer',viewerSession),
      ...Array.from({length:40},(_,i)=>i%2?host.read(room,'host',hostSession):viewerSignals.inbox(room,'viewer',viewerSession))
    ]);
    assert.equal(commits,2);assert.ok(conflicts>=1,'Fixture did not exercise a concurrent commit conflict');
    assert.equal(documents.get('rooms:'+room).revision,2);
    const stageCount=revisionStages,transactionCount=serial;
    await Promise.all(Array.from({length:40},(_,i)=>i%2?viewer.read(room,'viewer',viewerSession):hostSignals.inbox(room,'host',hostSession)));
    assert.equal(serial,transactionCount,'Idle polls created transactions');
    assert.equal(revisionStages,stageCount,'Idle polls staged revision writes');
    assert.equal(transactions.size,0);
  } finally {Client.prototype.call=original;}
});

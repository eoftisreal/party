import test from 'node:test';
import assert from 'node:assert/strict';
import {Client} from 'node-appwrite';
import {makeAppwriteStore} from '../src/store.js';
import {limits} from '../src/protocol.js';
import {coordinationIndexes} from '../src/schema.js';
test('transaction cache reuses isolated reads, invalidates mutations and never caches root authorization reads',async()=>{
  const original=Client.prototype.call;let gets=0;const documents=new Map();
  Client.prototype.call=async function(method,url,_headers,payload){
    if(url.pathname.endsWith('/databases/transactions'))return {$id:'tx'};
    if(url.pathname.includes('/databases/transactions/'))return {};
    const id=url.pathname.split('/').at(-1);
    if(method==='get'){++gets;return structuredClone(documents.get(id)||{$id:id,sessionId:'initial'});}
    const value={$id:id,...documents.get(id),...payload.data};documents.set(id,value);return structuredClone(value);
  };
  try {
    const store=makeAppwriteStore({APPWRITE_ENDPOINT:'https://example.test/v1',APPWRITE_PROJECT_ID:'test',PARTY_DATABASE_ID:'party'},'key');
    await store.atomic(async s=>{
      const first=await s.getMember('member');first.sessionId='caller-mutation';
      assert.equal((await s.getMember('member')).sessionId,'initial');assert.equal(gets,1);
      await s.updateMember('member',{sessionId:'renewed'});
      assert.equal((await s.getMember('member')).sessionId,'renewed');assert.equal(gets,2);
    });
    assert.equal((await store.getMember('member')).sessionId,'renewed');
    documents.set('member',{$id:'member',sessionId:'revoked'});
    assert.equal((await store.getMember('member')).sessionId,'revoked');assert.equal(gets,4);
  } finally {Client.prototype.call=original;}
});
test('SDK inbox and cleanup use bounded indexed recipient/expiry queries',async()=>{
  const original=Client.prototype.call,calls=[];
  Client.prototype.call=async function(_method,url,_headers,payload){calls.push({path:url.pathname,queries:payload.queries.map(q=>JSON.parse(q))});return {documents:[]};};
  try {
    const store=makeAppwriteStore({APPWRITE_ENDPOINT:'https://example.test/v1',APPWRITE_PROJECT_ID:'test',PARTY_DATABASE_ID:'party'},'key');
    const at=new Date().toISOString();
    await store.listInbox('room','viewer','session',at);
    const inbox=calls[0].queries;
    assert.deepEqual(inbox.filter(q=>q.method==='equal').map(q=>q.attribute),coordinationIndexes.signals.inbox.slice(0,3));
    assert.ok(inbox.some(q=>q.method==='greaterThan' && q.attribute==='expiresAt' && q.values[0]===at));
    await store.listCleanupRooms('closed',at);await store.listExpiredSignals(at);await store.listExpiredNotifications(at);
    for(const [index,maximum] of [[1,limits.cleanupRooms],[2,limits.cleanupExpired],[3,limits.cleanupExpired]]) {
      assert.ok(calls[index].queries.some(q=>q.method==='limit' && q.values[0]===maximum));
      assert.ok(calls[index].queries.some(q=>q.method==='lessThanEqual' && q.attribute==='expiresAt'));
    }
  } finally {Client.prototype.call=original;}
});
test('actual Appwrite SDK sends transactionId, private signal and receipt permissions',async()=>{
  const original=Client.prototype.call,calls=[];
  Client.prototype.call=async function(method,url,headers,payload){
    const path=url.pathname;calls.push({method,path,payload});
    if(path==='/v1/databases/transactions'&&method==='post')return {$id:'tx'};
    if(path.includes('/notifications/')&&method==='get')throw Object.assign(Error(),{code:404});
    if(path.endsWith('/documents')&&method==='get')return {documents:[]};
    return {$id:payload.documentId||'doc',...payload.data};
  };
  try{
    const store=makeAppwriteStore({APPWRITE_ENDPOINT:'https://example.test/v1',APPWRITE_PROJECT_ID:'test',PARTY_DATABASE_ID:'party'},'key');
    await store.atomic(async s=>{
      assert.equal(s.isTransaction,true);
      await s.createSignal('abc',{payload:'secret'});
      await s.notify('abc',{userId:'viewer',roomId:'room'});
      await s.notify('guest',{userId:'g_'+'a'.repeat(32),roomId:'room'});
      await s.listMembers('room');
    });
    const signal=calls.find(x=>x.path.includes('/signals/')&&x.method==='post');
    assert.equal(signal.payload.transactionId,'tx');assert.deepEqual(signal.payload.permissions,[]);
    const notification=calls.find(x=>x.path.includes('/notifications/')&&x.method==='post');
    assert.deepEqual(notification.payload.permissions,[]);
    const guestNotice=calls.find(x=>x.path.includes('/notifications/')&&x.method==='post'&&x.payload.documentId==='guest');
    assert.deepEqual(guestNotice.payload.permissions,[]);
    assert.equal(calls.at(-1).payload.commit,true);
    await assert.rejects(store.atomic(async()=>{throw Error('rollback');}));
    assert.equal(calls.at(-1).payload.rollback,true);
  }finally{Client.prototype.call=original;}
});

test('revision increment stays inside the transaction and conflict retries roll back before jittered replay',async()=>{
  const original=Client.prototype.call,events=[],delays=[];
  let transactions=0,commits=0;
  Client.prototype.call=async function(method,url,headers,payload){
    const path=url.pathname;
    if(path.endsWith('/databases/transactions')){events.push('create');return {$id:'tx'+(++transactions)};}
    if(path.includes('/databases/transactions/')) {
      if(payload.rollback){events.push('rollback');return {};}
      events.push('commit');
      if(++commits<3)throw Object.assign(Error('conflict'),{code:409,type:'transaction_conflict'});
      return {};
    }
    assert.ok(path.endsWith('/rooms/documents/room/revision/increment'));
    assert.equal(method,'patch');assert.equal(payload.value,1);
    assert.equal(payload.transactionId,'tx'+transactions);
    events.push('bump');return {$id:'room',revision:1};
  };
  try {
    const store=makeAppwriteStore({APPWRITE_ENDPOINT:'https://example.test/v1',APPWRITE_PROJECT_ID:'test',PARTY_DATABASE_ID:'party'},'key',{
      random:()=>0.5,wait:async ms=>{events.push('wait');delays.push(ms);}
    });
    await store.atomic(s=>s.bumpRoom('room'));
    assert.equal(transactions,3);assert.equal(commits,3);assert.deepEqual(delays,[62,125]);
    assert.deepEqual(events,['create','bump','commit','rollback','wait','create','bump','commit','rollback','wait','create','bump','commit']);
  } finally {Client.prototype.call=original;}
});

test('non-conflict database failures are not retried',async()=>{
  const original=Client.prototype.call;let starts=0,rollbacks=0;
  Client.prototype.call=async function(_method,url,_headers,payload){
    if(url.pathname.endsWith('/databases/transactions')){++starts;return {$id:'tx'};}
    if(payload.rollback){++rollbacks;return {};}
    throw Object.assign(Error('invalid'),{code:400});
  };
  try {
    const store=makeAppwriteStore({APPWRITE_ENDPOINT:'https://example.test/v1',APPWRITE_PROJECT_ID:'test',PARTY_DATABASE_ID:'party'},'key',{
      wait:async()=>assert.fail('Non-conflict failures must not back off/retry')
    });
    await assert.rejects(store.atomic(s=>s.bumpRoom('room')),e=>e.code===400);
    assert.equal(starts,1);assert.equal(rollbacks,1);
  } finally {Client.prototype.call=original;}
});

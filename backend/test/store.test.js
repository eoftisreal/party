import test from 'node:test';
import assert from 'node:assert/strict';
import {Client} from 'node-appwrite';
import {makeAppwriteStore} from '../src/store.js';
test('actual Appwrite SDK sends transactionId, private permissions and recipient-only notification permissions',async()=>{
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
    assert.deepEqual(notification.payload.permissions,['read("user:viewer")']);
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

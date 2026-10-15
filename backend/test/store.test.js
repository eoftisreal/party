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

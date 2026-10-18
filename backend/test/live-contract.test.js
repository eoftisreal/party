import test from 'node:test';
import assert from 'node:assert/strict';
import { Client, AppwriteException } from 'node-appwrite';
import main from '../src/main.js';
import { appwriteOperation, safeDiagnostic, logDiagnostic } from '../src/diagnostics.js';

// SDK requests are real; authentication/storage are offline contract fixtures.
// The transaction TTL validation matches Appwrite's tagged 2.4.0 source.
async function execution({ failAt, code = 400, type = 'document_invalid_structure', rollbackFails = false, createLimitPerHour = 0 } = {}) {
  const original = Client.prototype.call;
  const oldEnv = {...process.env};
  const calls = [], logs = [], persisted = new Map(), transactions = new Map();
  const secret = 'NEVER_LOG_JWT_API_KEY_INVITE_PASSWORD_SESSION';
  let serial = 0, response;
  const failure = () => new AppwriteException(secret, code, type, {password:secret});
  process.env.APPWRITE_ENDPOINT = 'https://example.test/v1';
  process.env.APPWRITE_PROJECT_ID = 'project';
  process.env.PARTY_DATABASE_ID = 'party-v2';
  delete process.env.PARTY_APPWRITE_API_KEY;
  if (createLimitPerHour === undefined) delete process.env.PARTY_CREATE_LIMIT_PER_HOUR;
  else process.env.PARTY_CREATE_LIMIT_PER_HOUR = String(createLimitPerHour);
  delete process.env.APPWRITE_FUNCTION_API_ENDPOINT;
  delete process.env.APPWRITE_FUNCTION_PROJECT_ID;
  for (const name of ['ROOMS','MEMBERS','PEERS','SIGNALS','NOTIFICATIONS','LIMITS']) delete process.env[`PARTY_${name}_COLLECTION_ID`];
  Client.prototype.call = async function(method, url, headers, payload) {
    const path = url.pathname;
    calls.push({method,path,payload});
    if (path === '/v1/account') return {$id:'host'};
    if (path === '/v1/databases/transactions') {
      if (failAt === 'transactions.create') throw failure();
      if (payload.ttl < 60 || payload.ttl > 3600) throw new AppwriteException('Invalid ttl',400,'general_argument_invalid');
      const id = `tx${++serial}`;
      transactions.set(id, structuredClone(persisted));
      return {$id:id};
    }
    if (path.startsWith('/v1/databases/transactions/')) {
      const transactionId = path.split('/').at(-1);
      if (payload.rollback) {
        if (rollbackFails) throw new AppwriteException(secret,503,'general_server_error');
        transactions.delete(transactionId); return {};
      }
      if (failAt === 'transactions.commit') throw failure();
      persisted.clear();
      for (const [key,value] of transactions.get(transactionId)) persisted.set(key,value);
      transactions.delete(transactionId);return {};
    }
    const match = /^\/v1\/databases\/party-v2\/collections\/(\w+)\/documents(?:\/([^/]+))?$/.exec(path);
    assert.ok(match, `Unexpected SDK endpoint ${path}`);
    const [,collection,id] = match;
    const action = ({get:'get',post:'create',patch:'update',delete:'delete'})[method];
    if (failAt === `documents.${action}:${collection}`) throw failure();
    assert.ok(payload.transactionId, 'Room path writes and reads must remain transactional');
    const documents = transactions.get(payload.transactionId);
    const key = collection+':'+(id || payload.documentId);
    if (method === 'get') {
      if (!documents.has(key)) throw new AppwriteException('missing',404,'document_not_found');
      return structuredClone(documents.get(key));
    }
    if (method === 'post') assert.deepEqual(payload.permissions,[], 'Persisted documents must remain private');
    const document = {$id:id || payload.documentId, ...documents.get(key), ...payload.data};
    documents.set(key,document);return structuredClone(document);
  };
  try {
    await main({req:{method:'POST',path:'/v1/rooms',headers:{'x-appwrite-key':secret},bodyJson:{title:'Movie night'}},
      res:{json:(body,status)=>{response={body,status};return response;}},error:line=>logs.push(line)});
    return {response,calls,logs,persisted,transactions,secret};
  } finally {
    Client.prototype.call = original;
    for (const key of Object.keys(process.env)) if (!(key in oldEnv)) delete process.env[key];
    Object.assign(process.env,oldEnv);
  }
}
test('authenticated room creation uses a valid Appwrite 2.4.0 TTL and commits room plus host membership', async () => {
  const {response,calls,persisted,transactions,logs} = await execution();
  assert.equal(response.status,201);
  const starts = calls.filter(c=>c.path === '/v1/databases/transactions');
  assert.equal(starts.length,1, 'Development-unlimited hosting skips the create budget transaction');
  assert.ok(starts.every(c=>c.payload.ttl === 60));
  const room = persisted.get('rooms:'+response.body.roomId);
  const members = [...persisted].filter(([key])=>key.startsWith('members:')).map(([,value])=>value);
  assert.match(room.hostId,/^g_[a-f0-9]{32}$/);assert.equal(members.length,1);
  assert.equal(members[0].roomId,room.$id);assert.equal(members[0].sessionId,response.body.sessionId);
  assert.equal([...persisted.keys()].filter(k=>k.startsWith('limits:')).length,0);
  assert.equal(transactions.size,0);assert.deepEqual(logs,[]);
});
test('optional creation cap still uses a durable transaction budget', async () => {
  const {response,calls,persisted,logs} = await execution({createLimitPerHour:4});
  assert.equal(response.status,201);
  assert.equal(calls.filter(c=>c.path === '/v1/databases/transactions').length,2);
  const limits=[...persisted].filter(([key])=>key.startsWith('limits:')).map(([,value])=>value);
  assert.equal(limits.length,1);
  assert.equal(limits[0].createCount,1);
  assert.equal([...persisted.keys()].filter(k=>k.startsWith('rooms:')).length,1);
  assert.deepEqual(logs,[]);
});
test('SDK transaction creation failure reports operation/status/type without secrets', async () => {
  const {response,logs,persisted,secret} = await execution({failAt:'transactions.create',type:'general_argument_invalid'});
  assert.equal(response.status,500);assert.equal(persisted.size,0);
  assert.deepEqual(JSON.parse(logs[0]),{event:'party_backend_error',operation:'transactions.create',status:400,errorType:'general_argument_invalid',errorName:'AppwriteException'});
  assert.ok(!JSON.stringify({logs,response}).includes(secret));
});
test('membership write failure rolls back the room and retains the original diagnostic if rollback fails', async () => {
  for (const rollbackFails of [false,true]) {
    const {response,calls,logs,persisted,secret} = await execution({failAt:'documents.create:members',rollbackFails});
    assert.equal(response.status,500);
    assert.equal([...persisted.keys()].filter(k=>/^(rooms|members):/.test(k)).length,0);
    assert.ok(calls.some(c=>c.payload?.rollback));
    const diagnostic = JSON.parse(logs[0]);
    assert.equal(diagnostic.operation,'documents.create:members');assert.equal(diagnostic.status,400);
    assert.ok(!JSON.stringify({logs,response}).includes(secret));
  }
});
test('commit conflicts keep bounded retries and report commit failure after exhaustion', async () => {
  const {response,calls,logs,persisted} = await execution({failAt:'transactions.commit',code:409,type:'document_update_conflict'});
  assert.equal(response.status,409);assert.equal(persisted.size,0);
  assert.equal(calls.filter(c=>c.path === '/v1/databases/transactions').length,4);
  assert.equal(JSON.parse(logs[0]).operation,'transactions.commit');
});
test('diagnostics reject unknown types/names and preserve the original error', async () => {
  const error = Object.assign(Error('secret-password'),{code:400,type:'secret-password',name:'secret-password',response:{jwt:'secret-password'}});
  await assert.rejects(appwriteOperation('documents.create:rooms',()=>Promise.reject(error)),e=>e === error);
  const diagnostic = safeDiagnostic(error);
  assert.equal(diagnostic.errorType,'unknown');assert.equal(diagnostic.errorName,'Error');
  assert.ok(!JSON.stringify(diagnostic).includes('secret-password'));
  assert.doesNotThrow(()=>logDiagnostic(()=>{throw Error('logger failed');},error));
});

test('guest creation uses real SDK storage without any Appwrite Account request',async()=>{
  const {response,calls,persisted}=await execution();
  assert.equal(response.status,201);assert.match(response.body.hostId,/^g_[a-f0-9]{32}$/);
  assert.equal(calls.some(c=>c.path==='/v1/account'),false);
  assert.equal(persisted.get('rooms:'+response.body.roomId).hostId,response.body.hostId);
});

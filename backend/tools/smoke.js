// Live Function test. Two different real Appwrite user JWTs are required.
import assert from 'node:assert/strict';
const base=process.env.PARTY_FUNCTION_URL,host=process.env.PARTY_HOST_JWT,viewer=process.env.PARTY_VIEWER_JWT;
if(!base?.startsWith('https://')||!host||!viewer)throw Error('Need HTTPS PARTY_FUNCTION_URL, PARTY_HOST_JWT and PARTY_VIEWER_JWT');
async function call(token,method,path,body,session){
  const response=await fetch(base+path,{method,headers:{Authorization:`Bearer ${token}`,'Content-Type':'application/json',...(session?{'X-Party-Session':session}:{})},...(body?{body:JSON.stringify(body)}:{})});
  return {status:response.status,body:await response.json()};
}
const created=await call(host,'POST','/v1/rooms',{title:'Live deployment smoke'});assert.equal(created.status,201);
const r=created.body,path='/v1/rooms/'+r.roomId;
try{
  assert.equal((await call('invalid','GET',path)).status,401);
  assert.equal((await call(viewer,'GET',path)).status,403);
  assert.equal((await call(viewer,'POST',path+'/join',{inviteCode:'x'.repeat(32)})).status,403);
  const joined=await call(viewer,'POST',path+'/join',{inviteCode:r.inviteCode});assert.equal(joined.status,200);
  const v=joined.body;assert.notEqual(v.yourUserId,r.yourUserId,'Smoke test needs two different accounts');
  assert.equal((await call(viewer,'POST',path+'/peers',{viewerId:r.hostId},v.sessionId)).status,403);
  const p=await call(host,'POST',path+'/peers',{viewerId:v.yourUserId},r.sessionId);assert.equal(p.status,200);
  assert.equal((await call(viewer,'GET',path+'/signals',undefined,v.sessionId)).status,200);
  assert.equal((await call(host,'POST',path+'/close',{},r.sessionId)).status,200);
  assert.equal((await call(viewer,'GET',path+'/signals',undefined,v.sessionId)).status,410);
  console.log('Live authentication, membership, host authorization, transaction and closure smoke passed. Media and Realtime require the native two-machine test.');
}finally{await call(host,'POST',path+'/close',{},r.sessionId);}

// Offline integration fixture, bound only to loopback. Authentication/database are fixtures,
// while routing, room/signaling authorization and native TLS/WebRTC paths are production code.
import https from 'node:https';
import fs from 'node:fs';
import {randomBytes} from 'node:crypto';
import {fixture} from './fixture.js';
import {makeHandler} from '../src/http.js';
const f=fixture();let dropSignal=true,dropAck=true;
const minted={host:0,viewer:0},validJwt=new Map();
const handler=makeHandler({service:f.rooms,signaling:f.signaling,authenticate:async token=>{
  if(validJwt.has(token))return validJwt.get(token);
  throw Object.assign(Error('auth'),{code:401});
}});
const counts={signals:0,acks:0,offers:0,answers:0,joins:0,closed:0,signalResponseDropped:0,ackResponseDropped:0,jwtRequests:0,expiredJwts:0};
const server=https.createServer({key:fs.readFileSync(process.env.TEST_KEY),cert:fs.readFileSync(process.env.TEST_CERT)},async(req,res)=>{
  const path=new URL(req.url,'https://localhost').pathname;
  if(path==='/account/jwts' && req.method==='POST'){
    const secret=req.headers['x-appwrite-session'];
    const user=secret===process.env.TEST_HOST_TOKEN?'host':secret===process.env.TEST_VIEWER_TOKEN?'viewer':null;
    if(!user){res.writeHead(401);res.end('{}');return;}
    const jwt=randomBytes(24).toString('base64url');++counts.jwtRequests;
    // Expire the first issued token for each account: native must renew once after a 401.
    if(++minted[user]>1)validJwt.set(jwt,user);else ++counts.expiredJwts;
    res.writeHead(201,{'Content-Type':'application/json'});res.end(JSON.stringify({jwt}));return;
  }
  if(path==='/test-metrics' && req.headers.authorization===`Bearer ${process.env.TEST_HOST_TOKEN}`){res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify(counts));return;}
  let text='';for await(const chunk of req){text+=chunk;if(Buffer.byteLength(text)>65536){res.writeHead(413);res.end('{}');return;}}
  let payload={};try{payload=text?JSON.parse(text):{};}catch{}
  if(path.endsWith('/signals') && req.method==='POST'){++counts.signals;if(payload.type==='offer')++counts.offers;if(payload.type==='answer')++counts.answers;}
  if(path.endsWith('/ack'))++counts.acks;if(path.endsWith('/join'))++counts.joins;
  await handler({req:{path,method:req.method,headers:req.headers,bodyText:text},res:{json(body,status){
    if(status<300 && path.endsWith('/signals') && req.method==='POST' && dropSignal){dropSignal=false;++counts.signalResponseDropped;req.socket.destroy();return;}
    if(status<300 && path.endsWith('/ack') && dropAck){dropAck=false;++counts.ackResponseDropped;req.socket.destroy();return;}
    if(body.error?.code==='ROOM_CLOSED')++counts.closed;
    res.writeHead(status,{'Content-Type':'application/json'});res.end(JSON.stringify(body));
  }}});
});
server.listen(0,'127.0.0.1',()=>console.log(JSON.stringify({port:server.address().port})));
function stop(){console.log(JSON.stringify(counts));server.close(()=>process.exit(0));setTimeout(()=>process.exit(0),2000).unref();}
process.on('SIGTERM',stop);process.on('SIGINT',stop);

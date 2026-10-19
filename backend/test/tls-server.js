// Offline integration fixture, bound only to loopback. Authentication/database are fixtures,
// while routing, room/signaling authorization and native TLS/WebRTC paths are production code.
import https from 'node:https';
import fs from 'node:fs';
import {fixture} from './fixture.js';
import {makeHandler} from '../src/http.js';
const f=fixture();let wallLast=Date.now(),outageUntil=0;
function tick(){const now=Date.now();f.advance(now-wallLast);wallLast=now;}
setInterval(tick,1000).unref();
let dropSignal=true,dropAck=true,dropStart=true,dropJoin=true,dropSync=true;
const handler=makeHandler({service:f.rooms,signaling:f.signaling});
const statuses={};
function record(path,method,status,code=''){const action=/^[a-f0-9]{32}$/.test(path.split('/').at(-1))?'room':path.split('/').at(-1);const name=method+' '+action+' '+status+' '+code;statuses[name]=(statuses[name]||0)+1;}
const counts={signals:0,acks:0,offers:0,answers:0,joins:0,closed:0,exchanges:0,exchangeMessages:0,exchangeAcks:0,syncResponseDropped:0,startResponseDropped:0,joinResponseDropped:0,signalResponseDropped:0,ackResponseDropped:0,accountRequests:0,guestRequests:0};
const server=https.createServer({key:fs.readFileSync(process.env.TEST_KEY),cert:fs.readFileSync(process.env.TEST_CERT)},async(req,res)=>{
  tick();const path=new URL(req.url,'https://localhost').pathname;
  if(path==='/test-outage' && req.headers.authorization===`Bearer ${process.env.TEST_HOST_TOKEN}`){
    outageUntil=Date.now()+102000;res.writeHead(200);res.end('{}');return;
  }
  if(path.startsWith('/account'))++counts.accountRequests;
  if(path==='/test-metrics' && req.headers.authorization===`Bearer ${process.env.TEST_HOST_TOKEN}`){res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify({...counts,statuses,outageRemainingMs:Math.max(0,outageUntil-Date.now()),members:[...f.ms.values()].map(m=>({user:m.userId,ageMs:f.rooms.now()-Date.parse(m.lastSeenAt)}))}));return;}
  if(Date.now()<outageUntil){record(path,req.method,503,'SERVICE_UNAVAILABLE');res.writeHead(503,{'Content-Type':'application/json'});res.end(JSON.stringify({error:{code:'SERVICE_UNAVAILABLE'}}));return;}
  if(path==='/v1/guest' && req.method==='POST')++counts.guestRequests;
  let text='';for await(const chunk of req){text+=chunk;if(Buffer.byteLength(text)>65536){res.writeHead(413);res.end('{}');return;}}
  let payload={};try{payload=text?JSON.parse(text):{};}catch{}
  if(path.endsWith('/signals') && req.method==='POST'){++counts.signals;if(payload.type==='offer')++counts.offers;if(payload.type==='answer')++counts.answers;}
  if(path.endsWith('/ack'))++counts.acks;if(path.endsWith('/join'))++counts.joins;
  if(path.endsWith('/sync') && req.method==='POST'){
    ++counts.exchanges;counts.exchangeMessages+=payload.messages?.length||0;counts.exchangeAcks+=payload.acknowledgements?.length||0;
    for(const m of payload.messages||[]){if(m.type==='offer')++counts.offers;if(m.type==='answer')++counts.answers;}
  }
  await handler({req:{path,method:req.method,headers:req.headers,bodyText:text},res:{json(body,status){
    record(path,req.method,status,body.error?.code||'');
    if(status<300 && path==='/v1/rooms' && dropStart){dropStart=false;++counts.startResponseDropped;req.socket.destroy();return;}
    if(status<300 && path.endsWith('/join') && dropJoin){dropJoin=false;++counts.joinResponseDropped;req.socket.destroy();return;}
    if(status<300 && path.endsWith('/signals') && req.method==='POST' && dropSignal){dropSignal=false;++counts.signalResponseDropped;req.socket.destroy();return;}
    if(status<300 && path.endsWith('/ack') && dropAck){dropAck=false;++counts.ackResponseDropped;req.socket.destroy();return;}
    if(status<300 && path.endsWith('/sync') && req.method==='POST' && payload.messages?.length && dropSync){dropSync=false;++counts.syncResponseDropped;req.socket.destroy();return;}
    if(body.error?.code==='ROOM_CLOSED')++counts.closed;
    res.writeHead(status,{'Content-Type':'application/json'});res.end(JSON.stringify(body));
  }}});
});
server.listen(0,'127.0.0.1',()=>console.log(JSON.stringify({port:server.address().port})));
function stop(){console.log(JSON.stringify(counts));server.close(()=>process.exit(0));setTimeout(()=>process.exit(0),2000).unref();}
process.on('SIGTERM',stop);process.on('SIGINT',stop);

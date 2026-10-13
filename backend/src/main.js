import {makeAppwriteStore,verifyUserJWT} from './store.js';
import {makeRoomsService} from './rooms.js';
import {makeHandler} from './http.js';
export default async function main(context){
 try{
 const store=makeAppwriteStore(process.env,context.req.headers?.['x-appwrite-key']);
 return makeHandler({service:makeRoomsService(store),authenticate:token=>verifyUserJWT(token,process.env),logError:e=>context.error?.('Room API error: '+(e?.name||'Error'))})(context);
 }catch{context.error?.('Room API startup failure');return context.res.json({error:{code:'SERVICE_UNAVAILABLE',message:'Service not configured'}},503);}
}

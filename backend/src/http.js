import {ApiError} from './rooms.js';
const route=/^\/v1\/rooms\/([a-f0-9]{32})(?:\/(join|leave|close))?$/;
function body(req){if(req.bodyText?.length>2048)throw new ApiError(413,'BODY_TOO_LARGE','Body too large');try{const v=req.bodyJson??(req.bodyText?JSON.parse(req.bodyText):{});if(!v||typeof v!=='object'||Array.isArray(v))throw Error();return v;}catch{throw new ApiError(400,'INVALID_JSON','Expected JSON object');}}
export function makeHandler({service,authenticate,logError=()=>{}}){return async({req,res})=>{try{
 const path=(req.path||'/').replace(/\/$/,'')||'/',method=(req.method||'').toUpperCase();
 if(path==='/health'&&method==='GET')return res.json({ok:true,service:'party-room-api',version:1},200);
 if(path!=='/v1/rooms'&&!route.test(path))throw new ApiError(404,'NOT_FOUND','Route not found');
 const auth=Object.entries(req.headers||{}).find(([k])=>k.toLowerCase()==='authorization')?.[1];
 const token=typeof auth==='string'&&/^Bearer ([A-Za-z0-9._~-]+)$/.exec(auth)?.[1];if(!token||token.length>8192)throw new ApiError(401,'AUTH_REQUIRED','Appwrite JWT required');
 let user;try{user=await authenticate(token);}catch(e){if(e.code===401||e.code===403)throw new ApiError(401,'INVALID_TOKEN','Invalid JWT');throw e;}
 if(!user)throw new ApiError(401,'INVALID_TOKEN','Invalid JWT');
 if(path==='/v1/rooms'&&method==='POST')return res.json(await service.create(user,body(req)),201);
 const [,id,action]=route.exec(path)||[];
 if(id&&method==='GET'&&!action)return res.json(await service.read(id,user),200);
 if(id&&method==='POST'&&action==='join')return res.json(await service.join(id,user,body(req).inviteCode),200);
 if(id&&method==='POST'&&action==='leave')return res.json(await service.leave(id,user),200);
 if(id&&method==='POST'&&action==='close')return res.json(await service.close(id,user),200);
 throw new ApiError(405,'METHOD_NOT_ALLOWED','Method not allowed');
 }catch(e){if(e instanceof ApiError)return res.json({error:{code:e.code,message:e.message}},e.status);logError(e);return res.json({error:{code:'INTERNAL_ERROR',message:'Request failed'}},500);}};}

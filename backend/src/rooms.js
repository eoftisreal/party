import { randomBytes, createHash, timingSafeEqual } from 'node:crypto';
export class ApiError extends Error { constructor(status,code,message){super(message);this.status=status;this.code=code;} }
const digest=s=>createHash('sha256').update(s).digest('hex');
const memberId=(room,user)=>digest(room+':'+user).slice(0,32);
const roomPattern=/^[a-f0-9]{32}$/;
function safe(room,members,user){return {roomId:room.$id,title:room.title,hostId:room.hostId,status:room.status,expiresAt:room.expiresAt,participants:members.map(m=>({userId:m.userId,joinedAt:m.joinedAt})),yourRole:room.hostId===user?'host':'viewer'};}
export function makeRoomsService(store,{now=()=>Date.now(),random=randomBytes}={}){
 async function get(id){if(!roomPattern.test(id||''))throw new ApiError(400,'INVALID_ROOM_ID','Invalid room identifier');const room=await store.getRoom(id);if(!room)throw new ApiError(404,'ROOM_NOT_FOUND','Room not found');return room;}
 function active(r){if(r.status!=='open'||Date.parse(r.expiresAt)<=now())throw new ApiError(410,'ROOM_CLOSED','Room closed or expired');}
 async function member(id,user){const r=await get(id);if(!await store.getMember(memberId(id,user)))throw new ApiError(403,'NOT_A_MEMBER','Not a room member');return r;}
 return {
 async create(user,input={}){const title=input.title===undefined?'Watch party':input.title;if(typeof title!=='string'||!title.trim()||title.trim().length>80)throw new ApiError(400,'INVALID_TITLE','Title must be 1 to 80 characters');const id=random(16).toString('hex'),inviteCode=random(24).toString('base64url');const room=await store.createRoom(id,{hostId:user,title:title.trim(),inviteHash:digest(inviteCode),status:'open',expiresAt:new Date(now()+86400000).toISOString()});const m={roomId:id,userId:user,joinedAt:new Date(now()).toISOString()};try{await store.createMember(memberId(id,user),m);}catch(e){try{await store.deleteRoom(id);}catch{}throw e;}return {...safe(room,[m],user),inviteCode};},
 async join(id,user,invite){const r=await get(id);active(r);const mid=memberId(id,user);if(!await store.getMember(mid)){if(typeof invite!=='string'||!/^[A-Za-z0-9_-]{32}$/.test(invite))throw new ApiError(403,'INVALID_INVITE','Invite code required');const a=Buffer.from(digest(invite),'hex'),b=Buffer.from(r.inviteHash,'hex');if(a.length!==b.length||!timingSafeEqual(a,b))throw new ApiError(403,'INVALID_INVITE','Invalid invite');try{await store.createMember(mid,{roomId:id,userId:user,joinedAt:new Date(now()).toISOString()});}catch(e){if(e.code!==409)throw e;}}return safe(r,await store.listMembers(id),user);},
 async read(id,user){const r=await member(id,user);active(r);return safe(r,await store.listMembers(id),user);},
 async leave(id,user){const r=await member(id,user);if(r.hostId===user){if(r.status==='open')await store.updateRoom(id,{status:'closed'});return {left:true,roomClosed:true};}await store.deleteMember(memberId(id,user));return {left:true,roomClosed:false};},
 async close(id,user){const r=await member(id,user);if(r.hostId!==user)throw new ApiError(403,'HOST_ONLY','Only host can close');if(r.status!=='closed')await store.updateRoom(id,{status:'closed'});return {closed:true};}
 };
}

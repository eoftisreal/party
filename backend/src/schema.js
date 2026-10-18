// Keep deployment validation importable without loading credentials or contacting Cloud.
export const schema = {
  rooms:{hostId:36,title:80,inviteHash:64,status:12,expiresAt:'datetime',revision:'integer'},
  members:{revoked:{size:8,xdefault:''},roomId:32,userId:36,joinedAt:'datetime',lastSeenAt:'datetime',sessionId:32,rateWindow:'integer',rateCount:'integer'},
  peers:{requestId:{size:32,xdefault:''},roomId:32,hostId:36,viewerId:36,hostSession:32,viewerSession:32,generation:32,state:16,expiresAt:'datetime'},
  signals:{roomId:32,from:36,to:36,generation:32,type:16,payload:65536,toSession:32,expiresAt:'datetime'},
  limits:{revision:'integer',createWindow:'integer',createCount:'integer',joinWindow:'integer',joinCount:'integer'},
  notifications:{fingerprint:{size:64,xdefault:''},roomId:32,userId:36,expiresAt:'datetime'}
};
export function validateAttribute(name, definition, attribute) {
  const type = typeof definition==='number' || typeof definition==='object' ? 'string' : definition;
  const size = typeof definition==='object' ? definition.size : definition;
  if(attribute.array || (typeof definition==='object' && attribute.default!==definition.xdefault) || attribute.type!==type || attribute.required!==(typeof definition!=='object') ||
     (type==='string' && attribute.size<size) ||
     (type==='integer' && ((attribute.min??0)>0 || (attribute.max??2147483647)<2147483647)))
    throw Error(`Incompatible attribute ${name}; migrate in a separate database, do not overwrite user data`);
}

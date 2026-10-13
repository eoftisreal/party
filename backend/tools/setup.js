// Creates a fresh schema. Does not delete databases, collections, or user data.
import {Client,Databases} from 'node-appwrite';
const endpoint=process.env.APPWRITE_ENDPOINT,project=process.env.APPWRITE_PROJECT_ID,key=process.env.PARTY_SETUP_API_KEY;
const databaseId=process.env.PARTY_DATABASE_ID||'party-v2';
if(!endpoint||!project||!key)throw Error('Set APPWRITE_ENDPOINT, APPWRITE_PROJECT_ID and PARTY_SETUP_API_KEY');
const db=new Databases(new Client().setEndpoint(endpoint).setProject(project).setKey(key));
const schema={
  rooms:{hostId:36,title:80,inviteHash:64,status:12,expiresAt:'datetime',revision:'integer'},
  members:{roomId:32,userId:36,joinedAt:'datetime',lastSeenAt:'datetime',sessionId:32,rateWindow:'integer',rateCount:'integer'},
  peers:{roomId:32,hostId:36,viewerId:36,hostSession:32,viewerSession:32,generation:32,state:16,expiresAt:'datetime'},
  signals:{roomId:32,from:36,to:36,generation:32,type:16,payload:65536,toSession:32,expiresAt:'datetime'},
  notifications:{roomId:32,userId:36,expiresAt:'datetime'}
};
try{await db.create({databaseId,name:'Party prototype'});}catch(e){if(e.code!==409)throw e;}
for(const [name,attributes]of Object.entries(schema)){
  const collectionId=process.env[`PARTY_${name.toUpperCase()}_COLLECTION_ID`]||name;
  try{await db.createCollection({databaseId,collectionId,name,permissions:[],documentSecurity:true});}catch(e){if(e.code!==409)throw e;}
  const collection=await db.getCollection({databaseId,collectionId});
  if(!collection.documentSecurity||collection.$permissions.length)throw Error(`Unsafe permissions on ${name}; remove collection permissions and enable document security`);
  for(const [key,type]of Object.entries(attributes)){
    try{
      const params={databaseId,collectionId,key,required:true};
      if(type==='datetime')await db.createDatetimeAttribute(params);
      else if(type==='integer')await db.createIntegerAttribute({...params,min:0,max:2147483647});
      else await db.createStringAttribute({...params,size:type});
    }catch(e){if(e.code!==409)throw e;}
    let available=false;
    for(let attempt=0;attempt<60;++attempt){
      const attr=await db.getAttribute({databaseId,collectionId,key});
      if(attr.status==='failed')throw Error(`Attribute failed: ${name}.${key}`);
      if(attr.status==='available'){available=true;break;}
      await new Promise(resolve=>setTimeout(resolve,1000));
    }
    if(!available)throw Error(`Attribute still pending: ${name}.${key}; retry setup`);
  }
  if(name!=='rooms'){
    try{await db.createIndex({databaseId,collectionId,key:'roomId',type:'key',attributes:['roomId']});}catch(e){if(e.code!==409)throw e;}
  }
  console.log(`Configured ${collectionId}`);
}
console.log('Wait for indexes to become available, then deploy backend. No live streaming test has been run by setup.');

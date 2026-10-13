import test from 'node:test';
import assert from 'node:assert/strict';
import {schema,validateAttribute} from '../src/schema.js';
test('schema migration checks existing types, requiredness and capacity',()=>{
  validateAttribute('peers.requestId',schema.peers.requestId,{type:'string',required:false,size:32,default:''});
  validateAttribute('rooms.revision','integer',{type:'integer',required:true,min:0,max:2147483647});
  for(const attribute of [
    {type:'string',required:true,size:32},{type:'string',required:false,size:16},
    {type:'integer',required:false,size:32},
    {type:'string',required:false,size:32,default:'wrong'},
    {type:'string',required:false,size:32,default:'',array:true}
  ])assert.throws(()=>validateAttribute('peers.requestId',schema.peers.requestId,attribute),/Incompatible/);
  assert.throws(()=>validateAttribute('signals.payload',65536,{type:'string',required:true,size:8192}),/Incompatible/);
});

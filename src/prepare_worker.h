#pragma once

// Runs only in the browser's dedicated worker. Extraction and PDT serialization
// belong to the production C++ module; this wrapper only moves bounded buffers.
const char PREPARE_WORKER_JS[] PROGMEM = R"WORKER('use strict';
let busy=false;
self.onmessage=async event=>{
 const request=event.data;
 if(!request||request.type!=='prepare'||typeof request.id!=='string'||!/^[0-9a-f]{32}$/.test(request.id))return;
 const id=request.id,file=request.file;
 const send=(type,fields={})=>self.postMessage({type,id,...fields});
 if(busy){send('error',{code:'busy',message:'Preparation is already running.'});return}
 if(!file||typeof file.arrayBuffer!=='function'||!Number.isInteger(file.size)||file.size<1||file.size>64*1048576||typeof file.name!=='string'||!/\.epub$/i.test(file.name)){
  send('error',{code:'input',message:'Choose an EPUB up to 64 MiB.'});return;
 }
 busy=true;let module;
 try{
  send('progress',{phase:'prepare'});
  importScripts('/prepare-core.js');
  module=await PaperDIYPrepare({locateFile:()=>'/prepare-core.wasm',print:()=>{},printErr:()=>{}});
  const input=module._pdt_allocate(file.size);
  if(!Number.isInteger(input)||input<=0||input+file.size>module.HEAPU8.length)throw new Error('Not enough memory to prepare this EPUB.');
  send('progress',{phase:'read'});
  let source=await file.arrayBuffer();
  if(source.byteLength!==file.size||input+source.byteLength>module.HEAPU8.length)throw new Error('Could not read the complete EPUB.');
  module.HEAPU8.set(new Uint8Array(source),input);source=null;
  send('progress',{phase:'prepare'});
  if(module._pdt_prepare(0)!==0){
   const error=module.UTF8ToString(module._pdt_error(),256);
   throw new Error(error||'Could not prepare this EPUB.');
  }
  const output=module._pdt_result(),size=module._pdt_result_size();
  if(!Number.isInteger(output)||output<=0||!Number.isInteger(size)||size<160||size>160+4194304+262160||output+size>module.HEAPU8.length)throw new Error('Invalid preparation result.');
  const buffer=module.HEAPU8.slice(output,output+size).buffer;
  send('progress',{phase:'done'});
  self.postMessage({type:'done',id,buffer,sourceName:file.name},[buffer]);
 }catch(error){send('error',{code:'prepare',message:typeof error.message==='string'?error.message:'Preparation is unavailable.'})}
 finally{if(module)module._pdt_release();busy=false}
};
)WORKER";

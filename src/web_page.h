#pragma once

const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PaperDIY</title>
<style>
body{font-family:system-ui,sans-serif;max-width:640px;margin:auto;padding:24px;color:#111}h1{font-size:24px;margin:0 0 8px}h2{font-size:18px;margin:28px 0 12px}label{display:block;margin:12px 0}input{display:block;width:100%;box-sizing:border-box;padding:10px;margin-top:6px;border:1px solid #aaa;border-radius:4px}button{padding:10px 18px;margin-top:8px}p{color:#555;font-size:14px}li{overflow-wrap:anywhere;margin:6px 0}progress{width:100%}
</style></head><body>
<h1>PaperDIY</h1><p id="status">Connecting…</p>
<h2>Upload book</h2>
<p>EPUB ≤ 64 MiB · TXT ≤ 4 MiB · Markdown ≤ 1 MiB</p>
<form id="uploadForm"><input id="file" type="file" accept=".epub,.txt,.md,.markdown" required>
<button id="uploadButton" type="submit" disabled>Upload</button></form>
<progress id="progress" max="1" value="0" hidden></progress><p id="uploadMsg"></p>
<h2>Home Wi-Fi</h2>
<form id="wifiForm"><label>SSID<input id="ssid" maxlength="32" autocomplete="off" required></label>
<label>Password<input id="pass" type="password" minlength="8" maxlength="63" autocomplete="new-password" required></label>
<button id="wifiButton" type="submit">Save</button></form><p id="wifiMsg"></p>
<h2>Books</h2><ul id="books"></ul>
<script>
history.replaceState(null,'','/');
const $=id=>document.getElementById(id);
let generation=null,sending=false,polling=false;
async function json(url,opt){const controller=new AbortController();const timer=setTimeout(()=>controller.abort(),12000);try{const r=await fetch(url,{...opt,signal:controller.signal});let j={};try{j=await r.json()}catch(e){if(r.ok)throw new Error('Incomplete response')}if(!r.ok){const err=new Error(j.error||('HTTP '+r.status));err.status=r.status;throw err}return j}finally{clearTimeout(timer)}}
function post(url,data){return json(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data)});}
function retryable(error){return !error.status||error.status===408||error.status>=500;}
async function retrySame(action){for(let attempt=0;;++attempt){try{return await action()}catch(error){if(attempt>=2||!retryable(error))throw error;await new Promise(resolve=>setTimeout(resolve,300*(attempt+1)))}}}
function newRequestId(){const bytes=new Uint8Array(16);crypto.getRandomValues(bytes);return Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join('');}
async function uploadState(id){return retrySame(()=>json('/api/upload/status?id='+encodeURIComponent(id)));}
async function refresh(){if(polling||sending)return;polling=true;try{const s=await json('/api/status');generation=s.uploadGeneration;$('status').textContent=s.sd?((s.busy?'Uploading · ':'')+Math.floor(s.free/1048576)+' MB free'):'SD card unavailable';$('uploadButton').disabled=s.busy||!s.sd;const ul=$('books');ul.textContent='';let offset=0;do{const fs=await json('/api/files?offset='+offset);for(const n of fs.files){const li=document.createElement('li');li.textContent=n;ul.appendChild(li)}offset=fs.next}while(offset!==null)}catch(e){$('status').textContent=e.message;$('uploadButton').disabled=true}finally{polling=false}}
$('uploadForm').addEventListener('submit',async e=>{
 e.preventDefault();const f=$('file').files[0];if(!f||sending||generation===null)return;
 sending=true;$('uploadButton').disabled=true;$('file').disabled=true;$('progress').hidden=false;$('progress').value=0;$('uploadMsg').textContent='Uploading…';let id=null;
 try{
  id=newRequestId();const metadata={name:f.name,size:f.size,generation,requestId:id};
  // Replays use the same request id and admission generation, never a new upload.
  const start=await retrySame(()=>post('/api/upload/start',metadata));
  if(start.id!==id)throw new Error('Unexpected upload identity');
  for(let offset=start.offset;offset<f.size;){
   const bytes=new Uint8Array(await f.slice(offset,offset+start.chunkSize).arrayBuffer());let raw='';for(const byte of bytes)raw+=String.fromCharCode(byte);
   const end=offset+bytes.length;let next=offset;
   for(let attempt=0;;++attempt){
    try{const result=await post('/api/upload/chunk',{id,offset,data:btoa(raw)});next=result.offset;if(next!==end)throw new Error('Unexpected upload offset');break}
    catch(error){if(!retryable(error))throw error;const state=await uploadState(id);if(state.state==='receiving'&&state.offset===end){next=end;break}if(state.state!=='receiving'||state.offset!==offset||attempt>=2)throw error}
   }
   offset=next;$('progress').value=offset/f.size;
  }
  const result=await retrySame(()=>post('/api/upload/commit',{id}));id=null;$('uploadMsg').textContent='Uploaded: '+result.name;$('file').value='';
 }catch(error){
  let recovered=false;
  if(id)try{const state=await uploadState(id);if(state.state==='complete'){$('uploadMsg').textContent='Uploaded: '+state.name;$('progress').value=1;$('file').value='';recovered=true}else if(state.state==='receiving')await post('/api/upload/cancel',{id})}catch(ignore){}
  if(!recovered)$('uploadMsg').textContent=error.message+' Check the book list before retrying.';
 }finally{sending=false;$('file').disabled=false;refresh()}
});
$('wifiForm').addEventListener('submit',async e=>{e.preventDefault();$('wifiButton').disabled=true;$('wifiMsg').textContent='Saving…';try{await post('/api/wifi',{ssid:$('ssid').value,pass:$('pass').value});$('pass').value='';$('wifiMsg').textContent='Saved. Select N on the reader to connect.'}catch(err){$('wifiMsg').textContent=err.message}finally{$('wifiButton').disabled=false}});
refresh();setInterval(refresh,3000);
</script></body></html>)HTML";

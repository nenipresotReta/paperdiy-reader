#pragma once

const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PaperDIY</title>
<style>
body{font-family:system-ui,sans-serif;max-width:640px;margin:auto;padding:24px;color:#111}h1{font-size:24px;margin:0 0 8px}h2{font-size:18px;margin:28px 0 12px}label{display:block;margin:12px 0}input{display:block;width:100%;box-sizing:border-box;padding:10px;margin-top:6px;border:1px solid #aaa;border-radius:4px}button{padding:10px 18px;margin-top:8px}p{color:#555;font-size:14px}li{overflow-wrap:anywhere;margin:6px 0}progress{width:100%}select{display:block;width:100%;padding:10px;margin-top:6px;background:white;border:1px solid #aaa;border-radius:4px}input[type=checkbox]{display:inline;width:auto;margin:0 8px 0 0}fieldset{border:0;padding:0;margin:0}details{margin:20px 0}summary{cursor:pointer}canvas{display:block;max-width:100%;height:260px;object-fit:contain;margin:12px auto;border:1px solid #ddd}button{cursor:pointer}button:disabled{cursor:default}[hidden]{display:none!important}
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
<h2>Reader settings</h2>
<form id="settingsForm"><fieldset id="settingsFields" disabled>
<label>Reading preset<select id="preset"><option value="0">Compact · 24 px</option><option value="1">Standard · 28 px</option><option value="2">Relaxed · 28 px</option></select></label>
<p>Presets include line spacing and page margins. Changes apply the next time you open a book, keeping your text position.</p>
<label><input id="hyphenation" type="checkbox">Automatic English hyphenation</label>
<label>Alignment<select id="justify"><option value="1">Justified</option><option value="0">Left aligned</option></select></label>
<details><summary>Advanced typography</summary>
<label><input id="punctuation" type="checkbox">Compress adjacent Chinese punctuation</label>
<label>Chinese–Latin spacing<select id="spacing"><option value="0">Off</option><option value="2">Compact</option><option value="4">Standard</option></select></label></details>
<label>Double-click interval (ms)<input id="doubleMs" type="number" min="100" max="400" step="1" required></label>
<label>Screen refresh<select id="refresh"><option value="0">Clarity</option><option value="1">Balanced</option><option value="2">Less flashing</option></select></label>
<label>Sleep background ink (%)<input id="ink" type="number" min="0" max="100" step="1" required></label>
<p>0 hides the previous page; 100 keeps it at full strength.</p>
<button id="settingsSave" type="submit">Save settings</button></fieldset></form><p id="settingsMsg" role="status"></p>
<h2>Sleep image</h2><p id="artState"></p>
<form id="artForm"><input id="artFile" type="file" accept="image/png,image/jpeg,image/webp" required><canvas id="artPreview" width="540" height="960" hidden></canvas><button id="artUpload" type="submit">Upload image</button></form>
<p>Processed locally in your browser. Black pixels overlay the page, aligned bottom right. An SD card is required.</p>
<button id="artRemove" type="button">Remove image</button> <button id="artDefault" type="button">Use firmware default</button><p id="artMsg" role="status"></p>
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
async function uploadFile(f,art=false){
 if(!f||sending||generation===null)return;
 const messageId=art?'artMsg':'uploadMsg';
 sending=true;$('settingsFields').disabled=true;$('artUpload').disabled=true;$('uploadButton').disabled=true;$('file').disabled=true;$('progress').hidden=false;$('progress').value=0;$(messageId).textContent='Uploading…';let id=null;
 try{
  id=newRequestId();const metadata={name:f.name,size:f.size,generation,requestId:id};if(art)metadata.kind='art';
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
  const result=await retrySame(()=>post('/api/upload/commit',{id}));id=null;$(messageId).textContent='Uploaded: '+result.name;$('file').value='';
 }catch(error){
  let recovered=false;
  if(id)try{const state=await uploadState(id);if(state.state==='complete'){$(messageId).textContent='Uploaded: '+state.name;$('progress').value=1;$('file').value='';recovered=true}else if(state.state==='receiving')await post('/api/upload/cancel',{id})}catch(ignore){}
  if(!recovered)$(messageId).textContent=error.message+' Check the book list before retrying.';
 }finally{sending=false;$('file').disabled=false;$('settingsFields').disabled=false;$('artUpload').disabled=false;refresh();if(art)await loadSettings(false)}
}
$('uploadForm').addEventListener('submit',async e=>{e.preventDefault();await uploadFile($('file').files[0]);});

$('wifiForm').addEventListener('submit',async e=>{e.preventDefault();$('wifiButton').disabled=true;$('wifiMsg').textContent='Saving…';try{await post('/api/wifi',{ssid:$('ssid').value,pass:$('pass').value});$('pass').value='';$('wifiMsg').textContent='Saved. Select N on the reader to connect.'}catch(err){$('wifiMsg').textContent=err.message}finally{$('wifiButton').disabled=false}});

function showArt(c){$('artState').textContent=c.art===3?'No image':c.art===0?'Firmware default':'Custom image on SD card';}
async function loadSettings(fill=true){try{const c=await json('/api/settings');if(fill){for(const k of ['preset','spacing','doubleMs','justify','refresh','ink'])$(k).value=String(c[k]);for(const k of ['punctuation','hyphenation'])$(k).checked=!!c[k];$('settingsFields').disabled=false;}showArt(c)}catch(e){$('settingsMsg').textContent=e.message;}}
$('settingsForm').addEventListener('submit',async e=>{
 e.preventDefault();if(sending)return;$('settingsFields').disabled=true;$('settingsMsg').textContent='Saving…';
 try{const data={};for(const k of ['preset','spacing','doubleMs','justify','refresh','ink'])data[k]=$(k).value;for(const k of ['punctuation','hyphenation'])data[k]=$(k).checked?'1':'0';const c=await post('/api/settings',data);showArt(c);$('settingsMsg').textContent='Saved.'}catch(e){$('settingsMsg').textContent=e.message}finally{$('settingsFields').disabled=false}
});
for(const [id,action] of [['artRemove','remove'],['artDefault','default']])$(id).addEventListener('click',async()=>{if(sending)return;$(id).disabled=true;try{showArt(await post('/api/artwork',{action}));$('artMsg').textContent='Saved.'}catch(e){$('artMsg').textContent=e.message}finally{$(id).disabled=false}});
function imageCrc(bytes){let v=0xffffffff;for(const b of bytes){v^=b;for(let i=0;i<8;i++)v=(v>>>1)^((v&1)?0xedb88320:0)}return (v^0xffffffff)>>>0;}
async function prepareImage(f,sequence){
 if(!f||!['image/png','image/jpeg','image/webp'].includes(f.type)||f.size>12*1048576)throw new Error('Choose a PNG, JPEG or WebP image up to 12 MiB.');
 const bitmap=await createImageBitmap(f);try{
 if(sequence!==undefined&&sequence!==artSequence)return null;
 const scale=Math.min(1,540/bitmap.width,960/bitmap.height),w=Math.max(1,Math.round(bitmap.width*scale)),h=Math.max(1,Math.round(bitmap.height*scale));
 const canvas=document.createElement('canvas');canvas.width=w;canvas.height=h;const ctx=canvas.getContext('2d');ctx.fillStyle='white';ctx.fillRect(0,0,w,h);ctx.drawImage(bitmap,0,0,w,h);
 const rgba=ctx.getImageData(0,0,w,h).data,stride=Math.ceil(w/8),bytes=new Uint8Array(16+stride*h),view=new DataView(bytes.buffer);bytes.set([80,68,73,49]);view.setUint16(4,w,true);view.setUint16(6,h,true);
 const preview=$('artPreview'),pc=preview.getContext('2d');pc.fillStyle='white';pc.fillRect(0,0,540,960);pc.fillStyle='black';
 for(let y=0;y<h;y++)for(let x=0;x<w;x++){const at=4*(y*w+x);if((299*rgba[at]+587*rgba[at+1]+114*rgba[at+2])/1000<160){bytes[16+y*stride+(x>>3)]|=128>>(x&7);pc.fillRect(540-w+x,960-h+y,1,1)}}
 view.setUint32(12,imageCrc(bytes.subarray(16)),true);preview.hidden=false;return new File([bytes],'shutdown.pdi',{type:'application/octet-stream'});
 }finally{bitmap.close()}
}
let preparedArt=null,artSequence=0;
$('artFile').addEventListener('change',async()=>{const seq=++artSequence;preparedArt=null;$('artUpload').disabled=true;try{const data=await prepareImage($('artFile').files[0],seq);if(seq===artSequence){preparedArt=data;$('artMsg').textContent='Preview ready.'}}catch(e){if(seq===artSequence)$('artMsg').textContent=e.message}finally{if(seq===artSequence)$('artUpload').disabled=false}});
$('artForm').addEventListener('submit',async e=>{e.preventDefault();if(!preparedArt){$('artMsg').textContent='Choose an image and wait for the preview.';return}await uploadFile(preparedArt,true);});
loadSettings();
refresh();setInterval(refresh,3000);
</script></body></html>)HTML";

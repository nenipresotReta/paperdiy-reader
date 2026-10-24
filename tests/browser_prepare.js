'use strict';
// Run the actual generated module, not a mock of the preparation algorithm.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const crypto = require('node:crypto');
const {Worker} = require('node:worker_threads');
const path = require('node:path');
const {performance} = require('node:perf_hooks');
const vm = require('node:vm');

async function main() {
  const manifest = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
  const factory = require(manifest.module);
  const module = await factory();
  let accepted = 0, rejected = 0;
  for (const item of manifest.cases) {
    const input = fs.readFileSync(item.source);
    const ptr = module._pdt_allocate(input.length);
    if (!ptr && !item.expected) {
      assert.ok(module.UTF8ToString(module._pdt_error()));
      assert.equal(module._pdt_result_size(),0); ++rejected; continue;
    }
    assert.ok(ptr, 'fixture allocation');
    module.HEAPU8.set(input, ptr);
    const status = module._pdt_prepare(item.flat ? 1 : 0);
    if (!item.expected) {
      assert.notEqual(status, 0, `rejected fixture ${path.basename(item.source)}`);
      assert.equal(module._pdt_result_size(), 0);
      assert.equal(module._pdt_result(), 0);
      assert.ok(module.UTF8ToString(module._pdt_error()));
      ++rejected;
    } else {
      assert.equal(status, 0, `${path.basename(item.source)}: ${module.UTF8ToString(module._pdt_error())}`);
      const result = Buffer.from(module.HEAPU8.subarray(module._pdt_result(), module._pdt_result() + module._pdt_result_size()));
      assert.deepEqual(result, fs.readFileSync(item.expected), 'whole PDT1 native/WASM byte parity');
      assert.deepEqual(result.subarray(32,64), crypto.createHash('sha256').update(input).digest());
      assert.deepEqual(result.subarray(64,96), crypto.createHash('sha256').update(result.subarray(160)).digest());
      assert.equal(result.length, 160 + result.readUInt32LE(24) + result.readUInt32LE(28));
      ++accepted;
    }
    module._pdt_release();
    assert.equal(module._pdt_result_size(), 0);
  }
  for (const bytes of [0, 21, 64 * 1024 * 1024 + 1, 0xffffffff]) {
    assert.equal(module._pdt_allocate(bytes), 0, 'input bound enforced in C ABI');
    assert.ok(module.UTF8ToString(module._pdt_error()));
  }
  assert.notEqual(module._pdt_prepare(0), 0, 'missing source fails');
  assert.notEqual(module._pdt_prepare(2), 0, 'unknown option fails');
  assert.ok(module.HEAPU8.length <= 128 * 1024 * 1024, 'heap remains bounded');

  // Exercise the production worker wrapper against real WASM as well. The
  // module bytes are supplied locally; HTTP/authentication has separate tests.
  const header=fs.readFileSync(manifest.workerHeader,'utf8');
  const workerSource=header.match(/R"WORKER\(([\s\S]*?)\)WORKER"/)[1];
  const sample=manifest.cases.find(item=>item.expected && !item.flat);
  const book=fs.readFileSync(sample.source), messages=[];
  let context;
  context=vm.createContext({
    Uint8Array, Number, Error,
    self:{postMessage(message){messages.push(message);}},
    importScripts(route){
      assert.equal(route,'/prepare-core.js');
      context.PaperDIYPrepare=options=>{
        assert.equal(options.locateFile(),'/prepare-core.wasm');
        return factory({...options,wasmBinary:fs.readFileSync(manifest.module.replace(/\.js$/,'.wasm'))});
      };
    }
  });
  vm.runInContext(workerSource,context);
  const request={type:'prepare',id:'a'.repeat(32),file:{name:'sample.epub',size:book.length,
    arrayBuffer:async()=>Uint8Array.from(book).buffer}};
  const running=context.self.onmessage({data:request});
  await context.self.onmessage({data:request});
  assert.ok(messages.some(m=>m.type==='error' && m.code==='busy'));
  await running;
  const done=messages.find(m=>m.type==='done');assert.ok(done,'production worker completed');
  assert.deepEqual(Buffer.from(done.buffer),fs.readFileSync(sample.expected));
  assert.ok(messages.some(m=>m.phase==='read') && messages.some(m=>m.phase==='prepare'));

  // Killing the worker must interrupt CPU-bound native preparation; an ordinary
  // cancellation message cannot be processed while synchronous C++ is running.
  const worker = new Worker(`
    const {parentPort, workerData}=require('node:worker_threads');
    require(workerData)().then(m=>{
      const p=m._pdt_allocate(64*1024*1024);
      if(!p)throw Error('cancel fixture allocation');
      m.HEAPU8.fill(0,p,p+64*1024*1024);
      parentPort.postMessage('started');
      m._pdt_prepare(0);
      parentPort.postMessage('completed');
    });`, {eval:true,workerData:manifest.module});
  let completed=false;
  const started = new Promise((resolve,reject)=>{
    worker.on('message',message=>{if(message==='started')resolve();else if(message==='completed')completed=true;});
    worker.once('error',reject);
  });
  const timeout=setTimeout(()=>worker.terminate(),5000);
  await started;
  const cancelAt=performance.now();await worker.terminate();clearTimeout(timeout);
  assert.ok(performance.now()-cancelAt<2000, 'hard cancellation is bounded');
  assert.equal(completed,false, 'no stale result after cancellation');
  if (manifest.oomModule) {
    const small=await require(manifest.oomModule)();
    assert.equal(small._pdt_allocate(16*1024*1024),0,'actual capped wasm heap allocation fails normally');
    assert.match(small.UTF8ToString(small._pdt_error()),/memory/);
    assert.notEqual(small._pdt_prepare(0),0);
    const textOomInput=small._pdt_allocate(13*1024*1024);assert.ok(textOomInput);
    small.HEAPU8.fill(0,textOomInput,textOomInput+13*1024*1024);
    assert.notEqual(small._pdt_prepare(0),0,'text allocation exhaustion fails normally');
    assert.match(small.UTF8ToString(small._pdt_error()),/memory/);
    assert.equal(small._pdt_result_size(),0);
    const source=fs.readFileSync(manifest.cases.find(c=>c.expected).source);
    const p=small._pdt_allocate(source.length);assert.ok(p);
    small.HEAPU8.set(source,p);assert.equal(small._pdt_prepare(0),0,'module recovers after OOM');
  }
  console.log(JSON.stringify({accepted,rejected,cancelled:true,oom:!!manifest.oomModule,heapBytes:module.HEAPU8.length}));
}
main().catch(error=>{console.error(error);process.exitCode=1;});

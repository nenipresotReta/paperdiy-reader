'use strict';

// Requires Node.js 18+; no npm packages or network/device access.
// Run: node tests/web_ui.js   (or: node --test tests/web_ui.js)
// This executes the production page's inline script, not a copied implementation.
// The fake server models protocol outcomes; network_integration.cpp independently
// tests the real C++ handlers. Timers advance retry delays immediately, so these
// cases do not validate real HTTP deadlines, browser rendering or Wi-Fi behavior.
if (Number(process.versions.node.split('.')[0]) < 18) {
  throw new Error('web_ui.js requires Node.js 18 or newer');
}

const assert = require('node:assert/strict');
const { webcrypto } = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { test } = require('node:test');
const vm = require('node:vm');

const sourcePath = path.join(__dirname, '..', 'src', 'web_page.h');
const firmwareSource = fs.readFileSync(sourcePath, 'utf8');
const page = firmwareSource.match(/const char INDEX_HTML\[\] PROGMEM = R"HTML\(([\s\S]*?)\)HTML";/);
assert.ok(page, 'production INDEX_HTML must be present');
const scripts = [...page[1].matchAll(/<script>([\s\S]*?)<\/script>/g)];
assert.equal(scripts.length, 1, 'review the harness if the page adds another script');
const productionScript = scripts[0][1];

class Element {
  constructor() {
    this.disabled = false;
    this.hidden = false;
    this.value = '';
    this.textContent = '';
    this.files = [];
    this.handlers = new Map();
    this.children = [];
  }
  addEventListener(event, handler) { this.handlers.set(event, handler); }
  appendChild(child) { this.children.push(child); }
  removeAttribute(name) { if (name === 'value') this.value = 0; }
}

// Deliberately small chunks exercise binary bytes, multiple offsets and a tail.
const BOOK_BYTES = Buffer.from([0, 1, 2, 253, 254, 255, 3, 4, 5]);
const BOOK_NAME = '中文.epub';
const CHUNK_SIZE = 4;

function jsonResponse(status, body) {
  return { status, ok: status >= 200 && status < 300, async json() { return body; } };
}

class ProtocolFixture {
  constructor(fault, prepared = false) {
    this.fault = fault;
    this.prepared = prepared;
    this.name = prepared ? BOOK_NAME + '.pdt' : BOOK_NAME;
    this.generation = 0;
    this.active = null;
    this.receipt = null;
    this.published = prepared && fault !== 'source-missing' ? [{name: BOOK_NAME, bytes: Buffer.from('existing source')}] : [];
    this.deliveries = [];
    this.startRequests = [];
    this.calls = new Map();
    this.acceptedChunks = [];
  }

  count(route) { return this.calls.get(route) || 0; }

  async fetch(url, options = {}) {
    const target = new URL(url, 'http://paperdiy.test');
    const route = target.pathname;
    const count = this.count(route) + 1;
    this.calls.set(route, count);
    assert.ok(count <= 20, `${route}: retries must be bounded`);
    assert.ok(options.signal instanceof AbortSignal, 'every fetch needs a deadline signal');

    if (route === '/api/settings') return jsonResponse(200,{punctuation:1,spacing:4,doubleMs:200,preset:1,hyphenation:1,justify:1,refresh:1,ink:50,art:0});
    if (route === '/api/status') {
      return jsonResponse(200, { uploadGeneration: this.generation, busy: !!this.active, sd: true, free: 1048576 });
    }
    if (route === '/api/files') {
      return jsonResponse(200, { files: this.published.map(file => file.name), next: null });
    }
    if (route === '/api/upload/status') {
      const id = target.searchParams.get('id');
      if (this.active?.id === id) {
        return jsonResponse(200, { state: 'receiving', offset: this.active.bytes.length, size: BOOK_BYTES.length });
      }
      if (this.receipt?.id === id) {
        return jsonResponse(200, { state: 'complete', offset: BOOK_BYTES.length, size: BOOK_BYTES.length, name: this.name });
      }
      return jsonResponse(409, { error: 'Upload expired or is unknown.' });
    }

    assert.equal(options.method, 'POST');
    assert.equal(options.headers['Content-Type'], 'application/x-www-form-urlencoded');
    const args = Object.fromEntries(options.body);
    let response;
    if (route === '/api/upload/start') {
      this.startRequests.push(args);
      assert.match(args.requestId, /^[a-f0-9]{32}$/);
      assert.equal(args.name, this.name);
      assert.equal(args.kind, this.prepared ? 'prepared' : undefined);
      if(this.fault === 'source-missing') return jsonResponse(409, {error: 'Upload the matching EPUB first.'});
      assert.equal(Number(args.size), BOOK_BYTES.length);
      if (!this.active && !this.receipt) {
        assert.equal(Number(args.generation), this.generation);
        this.active = { id: args.requestId, metadata: args, bytes: Buffer.alloc(0) };
        ++this.generation;
      } else {
        assert.deepEqual(args, (this.active || this.receipt).metadata, 'start retry must retain its identity and admission generation');
      }
      response = jsonResponse(200, { id: args.requestId, offset: this.active?.bytes.length ?? BOOK_BYTES.length, chunkSize: CHUNK_SIZE });
    } else if (route === '/api/upload/chunk') {
      if (this.fault === 'chunk-before') throw new TypeError('Request did not reach the device');
      assert.ok(this.active, 'a chunk must belong to an active upload');
      assert.equal(args.id, this.active.id);
      assert.equal(Number(args.offset), this.active.bytes.length, 'accepted data must not be sent twice');
      const data = Buffer.from(args.data, 'base64');
      assert.ok(data.length > 0 && data.length <= CHUNK_SIZE);
      this.acceptedChunks.push({ offset: Number(args.offset), data });
      this.active.bytes = Buffer.concat([this.active.bytes, data]);
      response = jsonResponse(200, { offset: this.active.bytes.length });
    } else if (route === '/api/upload/commit') {
      if (!this.receipt) {
        assert.ok(this.active);
        assert.equal(args.id, this.active.id);
        assert.deepEqual(this.active.bytes, BOOK_BYTES);
        this.receipt = this.active;
        this.deliveries.push({ name: this.name, bytes: this.active.bytes });
        if(!this.prepared) this.published.push({ name: this.name, bytes: this.active.bytes });
        this.active = null;
      }
      assert.equal(args.id, this.receipt.id, 'commit retry must use the same transaction');
      response = jsonResponse(200, { ok: true, name: this.name });
    } else if (route === '/api/upload/cancel') {
      assert.ok(this.active);
      assert.equal(args.id, this.active.id, 'cleanup must only cancel the owned transaction');
      this.active = null;
      return jsonResponse(200, { ok: true });
    } else {
      assert.fail(`Unexpected request: ${route}`);
    }

    // Apply the operation first, then lose only its response. This is the
    // ambiguous-success failure that ordinary retries must handle correctly.
    const loseResponse =
      (this.fault === 'start' && route.endsWith('/start') && count === 1) ||
      (this.fault === 'chunk' && route.endsWith('/chunk') && count === 1) ||
      (this.fault === 'commit' && route.endsWith('/commit') && count === 1) ||
      (this.fault === 'commit-all' && route.endsWith('/commit'));
    if (loseResponse) throw new TypeError('Response lost after operation succeeded');
    return response;
  }
}

async function runScenario(fault, prepared = false) {
  const uploadName = prepared ? BOOK_NAME + '.pdt' : BOOK_NAME;
  const elements = new Map();
  function element(id) {
    if (!elements.has(id)) elements.set(id, new Element());
    return elements.get(id);
  }
  element('file').files = [{
    name: uploadName,
    size: BOOK_BYTES.length,
    slice(start, end) {
      // ArrayBuffer must cover precisely this slice, without Buffer's pool.
      const bytes = Uint8Array.from(BOOK_BYTES.subarray(start, end));
      return { async arrayBuffer() { return bytes.buffer; } };
    },
  }];
  const fixture = new ProtocolFixture(fault, prepared);
  let timerId = 0;
  const timers = new Set();
  const context = {
    document: { getElementById: element, createElement() { return new Element(); } },
    history: { replaceState() {} },
    fetch: fixture.fetch.bind(fixture),
    URLSearchParams, AbortController, crypto: webcrypto, Uint8Array,
    btoa: value => Buffer.from(value, 'binary').toString('base64'),
    setInterval() {}, // Only initial refresh is needed; no wall-clock polling.
    setTimeout(callback, delay) {
      const id = ++timerId;
      timers.add(id);
      if (delay < 1000) queueMicrotask(() => { if (timers.delete(id)) callback(); });
      return id;
    },
    clearTimeout(id) { timers.delete(id); },
  };
  vm.createContext(context);
  vm.runInContext(productionScript, context, { filename: sourcePath, timeout: 1000 });
  await new Promise(setImmediate); // Finish the initial status/files refresh.
  assert.equal(element('uploadButton').disabled, false);
  const submit = element('uploadForm').handlers.get('submit');
  assert.equal(typeof submit, 'function');
  await submit({ preventDefault() {} });
  await new Promise(setImmediate); // Finish the refresh scheduled in finally.

  if(fault === 'source-missing') {
    assert.equal(fixture.generation, 0);
    assert.equal(fixture.count('/api/upload/start'), 1, 'an admission conflict is not retried');
    assert.equal(fixture.count('/api/upload/chunk'), 0);
    assert.equal(fixture.count('/api/upload/commit'), 0);
    assert.equal(fixture.deliveries.length, 0);
    assert.match(element('uploadMsg').textContent, /Upload the matching EPUB first/);
    assert.equal(element('file').disabled, false);
    assert.equal(element('uploadButton').disabled, false);
    assert.equal(timers.size, 0);
    return;
  }
  if(prepared) {
    assert.deepEqual(fixture.published, [{name: BOOK_NAME, bytes: Buffer.from('existing source')}]);
  }
  assert.equal(fixture.generation, 1, 'recovery must not admit another transaction');
  assert.equal(new Set(fixture.startRequests.map(args => args.requestId)).size, 1);
  assert.equal(element('file').disabled, false, 'release the file control after finishing');
  assert.equal(element('uploadButton').disabled, false, 'release upload after refreshing state');
  assert.equal(timers.size, 0, 'each request must clear its deadline timer');

  if (fault === 'chunk-before') {
    assert.equal(fixture.deliveries.length, 0);
    assert.equal(fixture.active, null);
    assert.equal(fixture.count('/api/upload/chunk'), 3, 'failed chunk retries must be bounded');
    assert.equal(fixture.count('/api/upload/cancel'), 1);
    assert.equal(fixture.count('/api/upload/commit'), 0);
    assert.match(element('uploadMsg').textContent, prepared ? /The original EPUB is unchanged\. You can retry\./ : /Check the book list/);
  } else {
    assert.equal(fixture.deliveries.length, 1);
    assert.deepEqual(fixture.deliveries[0].bytes, BOOK_BYTES);
    assert.deepEqual(fixture.acceptedChunks.map(chunk => chunk.offset), [0, 4, 8]);
    assert.equal(fixture.count('/api/upload/cancel'), 0);
    assert.equal(element('progress').value, 1);
    assert.equal(element('uploadMsg').textContent, 'Uploaded: ' + uploadName);
    assert.equal(fixture.count('/api/upload/start'), fault === 'start' ? 2 : 1);
    assert.equal(fixture.count('/api/upload/commit'), fault === 'commit-all' ? 3 : fault === 'commit' ? 2 : 1);
    if (fault === 'chunk' || fault === 'commit-all') assert.ok(fixture.count('/api/upload/status') >= 1);
  }
}

for (const [fault, description] of [
  ['none', 'uploads binary chunks and a tail normally'],
  ['start', 'retries a lost admission response with the same identity'],
  ['chunk', 'queries accepted offset after losing a chunk response'],
  ['commit', 'retries a lost commit response without another import'],
  ['commit-all', 'recovers the completed receipt after all commit responses are lost'],
  ['chunk-before', 'bounds failed chunk retries and cancels only its own upload'],
]) {
  test(description, { timeout: 2000 }, () => runScenario(fault));
}

for (const fault of ['none', 'start', 'chunk', 'commit', 'commit-all', 'chunk-before']) {
  test(`prepared EPUB delivery keeps the source library and recovers ${fault}`, { timeout: 2000 }, () => runScenario(fault, true));
}
test('prepared upload requires its source EPUB before sending chunks', { timeout: 2000 }, () => runScenario('source-missing', true));

test('settings use saved values, retain edits on failure, and image actions are explicit', async () => {
  const elements = new Map();const el=id=>{if(!elements.has(id))elements.set(id,new Element());return elements.get(id)};
  let config={punctuation:1,spacing:4,doubleMs:200,preset:1,hyphenation:1,justify:1,refresh:1,ink:50,art:0};
  let fail=false;const posts=[];
  const context={document:{getElementById:el,createElement:()=>new Element()},history:{replaceState(){}},URLSearchParams,AbortController,crypto:webcrypto,Uint8Array,
    setTimeout(){return 1},clearTimeout(){},setInterval(){},
    fetch:async(url,opt={})=>{
      if(url==='/api/status')return jsonResponse(200,{sd:true,free:10000,uploadGeneration:0});
      if(url.startsWith('/api/files'))return jsonResponse(200,{files:[],next:null});
      if(opt.method==='POST'){
        const data=Object.fromEntries(opt.body);posts.push({url,data});if(fail)return jsonResponse(500,{error:'Storage unavailable'});
        if(url==='/api/settings')config={...config,...Object.fromEntries(Object.entries(data).map(([k,v])=>[k,Number(v)]))};
        if(url==='/api/artwork')config.art=data.action==='remove'?3:0;
      }
      return jsonResponse(200,config);
    }};
  vm.createContext(context);vm.runInContext(productionScript,context);await new Promise(setImmediate);
  assert.equal(el('doubleMs').value,'200');assert.equal(el('punctuation').checked,true);
  el('doubleMs').value='210';el('spacing').value='2';el('punctuation').checked=false;
  const submit=el('settingsForm').handlers.get('submit');fail=true;await submit({preventDefault(){}});
  assert.equal(el('settingsMsg').textContent,'Storage unavailable');assert.equal(el('doubleMs').value,'210');assert.equal(config.doubleMs,200);assert.equal(el('settingsFields').disabled,false);
  fail=false;await submit({preventDefault(){}});assert.equal(config.doubleMs,210);assert.equal(config.spacing,2);assert.equal(config.punctuation,0);
  assert.equal(el('settingsMsg').textContent,'Saved.');assert.equal(posts.at(-1).url,'/api/settings');
  await el('artRemove').handlers.get('click')();assert.equal(config.art,3);assert.equal(el('artState').textContent,'No image');
  await el('artDefault').handlers.get('click')();assert.equal(config.art,0);
});

test('image conversion packs black bits, authenticates payload, and aligns preview bottom right', async()=>{
  const elements=new Map(),paint=[];let closed=false;
  const el=id=>{if(!elements.has(id))elements.set(id,new Element());return elements.get(id)};
  const preview={fillStyle:'',fillRect(...args){if(this.fillStyle==='black')paint.push(args)}};
  el('artPreview').getContext=()=>preview;
  const pixels=new Uint8ClampedArray([0,0,0,255,255,255,255,255,0,0,0,255,255,255,255,255,0,0,0,255,255,255,255,255]);
  const context={document:{getElementById:el,createElement:tag=>tag==='canvas'?{getContext:()=>({fillRect(){},drawImage(){},getImageData:()=>({data:pixels})})}:new Element()},
    history:{replaceState(){}},URLSearchParams,AbortController,Uint8Array,DataView,File:require('node:buffer').File,crypto:webcrypto,
    createImageBitmap:async()=>({width:3,height:2,close(){closed=true}}),setTimeout(){return 1},clearTimeout(){},setInterval(){},
    fetch:async url=>jsonResponse(200,url==='/api/settings'?{punctuation:1,spacing:4,doubleMs:200,preset:1,hyphenation:1,justify:1,refresh:1,ink:50,art:0}:url==='/api/status'?{sd:true,free:1000,uploadGeneration:0}:{files:[],next:null})};
  vm.createContext(context);vm.runInContext(productionScript,context);await new Promise(setImmediate);
  const image=await context.prepareImage({type:'image/png',size:100});const bytes=Buffer.from(await image.arrayBuffer());
  assert.equal(bytes.length,18);assert.equal(bytes.subarray(0,4).toString(),'PDI1');assert.equal(bytes.readUInt16LE(4),3);assert.equal(bytes.readUInt16LE(6),2);
  assert.deepEqual([...bytes.subarray(16)],[0xa0,0x40]);assert.equal(bytes.readUInt32LE(12),0x9902ef86);
  assert.deepEqual(paint,[[537,958,1,1],[539,958,1,1],[538,959,1,1]]);assert.ok(closed);
  await assert.rejects(context.prepareImage({type:'image/png',size:13*1048576}),/12 MiB/);
  await assert.rejects(context.prepareImage({type:'text/html',size:100}),/PNG/);
});

// Phone flow uses the same inline production UI, with a worker boundary and
// an independently modelled two-transaction server. Actual WASM extraction is
// covered by the importer/WASM parity tests; no browser-specific extraction is
// reimplemented here.
async function phoneScenario(options = {}) {
  const elements = new Map(), workers = [], timers = new Map(), receipts = new Map();
  const el = id => { if (!elements.has(id)) elements.set(id, new Element()); return elements.get(id); };
  const source = Buffer.from('Original EPUB bytes, unmodified.');
  const bundle = Buffer.alloc(192, 11); bundle.write('PDT1');
  const originalName = 'Test.epub', savedName = 'Test-1.epub';
  const published = new Map([[originalName, Buffer.from('existing book')]]), sidecars = new Map();
  const starts = [], cancels = [], requests = [];
  let generation = 0, active = null, timerId = 0, mainReads = 0;
  const file = {
    name: originalName, size: source.length,
    async arrayBuffer() { ++mainReads; return Uint8Array.from(source).buffer; },
    slice(start, end) { return new Blob([source.subarray(start, end)]); },
  };
  el('file').files = [file];
  el('file').value = 'picked.epub';
  class WorkerMock {
    constructor(url) {
      assert.equal(url, '/prepare-worker.js');
      if (options.worker === 'constructor-error') throw new Error('Worker unavailable');
      this.terminated = false; workers.push(this);
    }
    terminate() { this.terminated = true; }
    postMessage(message) {
      assert.equal(message.type, 'prepare'); assert.equal(message.file, file);
      this.request = message;
      queueMicrotask(() => {
        this.onmessage({data: {type: 'progress', id: message.id, phase: 'read'}});
        if (options.worker === 'stall') return;
        if (options.worker === 'error') return this.onmessage({data: {type: 'error', id: message.id, message: 'Unsupported EPUB.'}});
        if (options.worker === 'runtime-error') return this.onerror(new Error('WASM unavailable'));
        this.onmessage({data: {type: 'done', id: message.id, buffer: Uint8Array.from(options.worker === 'bad-result' ? [] : bundle).buffer}});
      });
    }
  }
  const fetch = async (url, opt = {}) => {
    const parsed = new URL(url, 'http://paperdiy.test');
    const route = parsed.pathname; requests.push(route);
    assert.ok(opt.signal instanceof AbortSignal);
    if (route === '/api/settings') return jsonResponse(200, {punctuation:1,spacing:4,doubleMs:200,preset:1,hyphenation:1,justify:1,refresh:1,ink:50,art:0});
    if (route === '/api/status') return jsonResponse(200, {sd:true,free:1e7,busy:!!active,uploadGeneration:generation,preprocessing:true});
    if (route === '/api/files') return jsonResponse(200, {files:[...published.keys()],next:null});
    if (route === '/api/upload/status') {
      const id = parsed.searchParams.get('id');
      if (receipts.has(id)) return jsonResponse(200, {state:'complete',name:receipts.get(id).name});
      if (active?.id === id) return jsonResponse(200, {state:'receiving',offset:active.bytes.length});
      return jsonResponse(409, {error:'Upload expired or is unknown.'});
    }
    assert.equal(opt.method, 'POST');
    const args = Object.fromEntries(opt.body);
    if (route === '/api/upload/start') {
      starts.push(args);
      assert.match(args.requestId, /^[a-f0-9]{32}$/);
      if (active?.id === args.requestId || receipts.has(args.requestId)) {
        const prior = active?.id === args.requestId ? active : receipts.get(args.requestId);
        assert.deepEqual(args, prior.metadata);
        return jsonResponse(200, {id:args.requestId,offset:prior.bytes.length,chunkSize:256});
      }
      assert.equal(Number(args.generation), generation, 'paired upload must fetch the new admission generation');
      assert.equal(active, null);
      assert.equal(args.name, args.kind === 'prepared' ? savedName + '.pdt' : originalName);
      if (args.kind === 'prepared') assert.deepEqual(published.get(savedName), source, 'source must commit first');
      active = {id:args.requestId,metadata:args,bytes:Buffer.alloc(0)}; ++generation;
      if (options.cancelAfterStart) el('cancelUpload').handlers.get('click')();
      return jsonResponse(200, {id:args.requestId,offset:0,chunkSize:256});
    }
    if (route === '/api/upload/chunk') {
      assert.equal(args.id, active?.id);
      if (active.metadata.kind === 'prepared' && options.sidecarFailure) return jsonResponse(500, {error:'Temporary write failure'});
      assert.equal(Number(args.offset), active.bytes.length);
      active.bytes = Buffer.concat([active.bytes, Buffer.from(args.data,'base64')]);
      return jsonResponse(200, {offset:active.bytes.length});
    }
    if (route === '/api/upload/commit') {
      let receipt = receipts.get(args.id);
      if (!receipt) {
        assert.equal(args.id, active?.id);
        const prepared = active.metadata.kind === 'prepared';
        assert.deepEqual(active.bytes, prepared ? bundle : source);
        const name = prepared ? savedName + '.pdt' : savedName;
        (prepared ? sidecars : published).set(name, active.bytes);
        receipt = {...active, name}; receipts.set(args.id, receipt); active = null;
      }
      if (!receipt.metadata.kind && options.cancelAfterOriginalCommit) el('cancelUpload').handlers.get('click')();
      if (!receipt.metadata.kind && options.loseOriginalCommit) throw new TypeError('Lost commit response');
      return jsonResponse(200, {name:receipt.name});
    }
    if (route === '/api/upload/cancel') {
      assert.equal(args.id, active?.id); cancels.push(args.id); active = null;
      return jsonResponse(200, {ok:true});
    }
    assert.fail('Unexpected network request: ' + route);
  };
  const context = {document:{getElementById:el,createElement:()=>new Element()},history:{replaceState(){}},fetch,
    URLSearchParams,AbortController,crypto:webcrypto,Uint8Array,ArrayBuffer,Blob,File:require('node:buffer').File,WebAssembly,
    btoa:value=>Buffer.from(value,'binary').toString('base64'),setInterval(){},
    setTimeout(callback,delay){const id=++timerId;timers.set(id,{callback,delay});if(delay<1000)queueMicrotask(()=>{if(timers.delete(id))callback()});return id},
    clearTimeout(id){timers.delete(id)},
  };
  if (!options.noWorker) context.Worker = WorkerMock;
  vm.createContext(context);vm.runInContext(productionScript,context);await new Promise(setImmediate);
  return {el,file,workers,starts,cancels,requests,published,sidecars,timers,source,bundle,savedName,
    mainReads:()=>mainReads,
    submit:()=>el('uploadForm').handlers.get('submit')({preventDefault(){}}),
    async settled(){await new Promise(setImmediate)},
    expirePreparation(){const entry=[...timers.entries()].find(([,v])=>v.delay===120000);assert.ok(entry);timers.delete(entry[0]);entry[1].callback()},
  };
}

test('phone EPUB preparation uploads the original first, then pairs with the server-returned name', async()=>{
  const h=await phoneScenario();
  assert.equal(h.el('prepareBook').checked,true); assert.equal(h.el('prepareBook').disabled,false);
  await h.submit();await h.settled();
  assert.equal(h.workers.length,1);assert.ok(h.workers[0].terminated);assert.equal(h.mainReads(),0,'main thread never reads the whole EPUB');
  assert.equal(h.starts.length,2);assert.equal(h.starts[0].kind,undefined);assert.equal(h.starts[1].kind,'prepared');
  assert.equal(h.starts[1].name,h.savedName+'.pdt');assert.equal(h.starts[1].generation,'1');
  assert.deepEqual(h.published.get(h.savedName),h.source);assert.deepEqual(h.published.get('Test.epub'),Buffer.from('existing book'));
  assert.deepEqual(h.sidecars.get(h.savedName+'.pdt'),h.bundle);
  assert.equal(h.el('uploadMsg').textContent,'Uploaded: '+h.savedName+' · Faster opening ready.');
  assert.equal(h.el('file').disabled,false);assert.equal(h.el('cancelUpload').hidden,true);assert.equal(h.timers.size,0);
});

test('paired preparation recovers lost original commit receipts without importing a duplicate', async()=>{
  const h=await phoneScenario({loseOriginalCommit:true});await h.submit();await h.settled();
  assert.equal(h.starts.filter(s=>!s.kind).length,1);assert.equal(h.published.size,2);assert.equal(h.sidecars.size,1);
  assert.equal(h.starts[1].name,h.savedName+'.pdt');assert.match(h.el('uploadMsg').textContent,/Faster opening ready/);
});

for(const worker of ['error','constructor-error','runtime-error','bad-result']) {
  test(`phone ${worker} keeps the original available for explicit fallback`,async()=>{
    const h=await phoneScenario({worker});await h.submit();await h.settled();
    assert.equal(h.starts.length,0);assert.equal(h.el('uploadOriginal').hidden,false);assert.match(h.el('uploadMsg').textContent,/Upload the original or retry/);
    assert.equal(h.el('file').value,'picked.epub','preparation failure leaves the original selection available');
    await h.el('uploadOriginal').handlers.get('click')();await h.settled();
    assert.equal(h.starts.length,1);assert.equal(h.starts[0].kind,undefined);assert.deepEqual(h.published.get(h.savedName),h.source);assert.equal(h.sidecars.size,0);
    assert.equal(h.timers.size,0);
  });
}

test('unsupported browser visibly disables preparation while original upload still works',async()=>{
  const h=await phoneScenario({noWorker:true});assert.equal(h.el('prepareBook').disabled,true);assert.match(h.el('prepareNote').textContent,/unavailable.*Original books/);
  await h.submit();assert.equal(h.starts.length,1);assert.equal(h.sidecars.size,0);
});

test('preparation can be explicitly turned off without changing the book upload',async()=>{
  const h=await phoneScenario();h.el('prepareBook').checked=false;await h.submit();
  assert.equal(h.workers.length,0);assert.equal(h.starts.length,1);assert.deepEqual(h.published.get(h.savedName),h.source);
});

test('cancelling a preparation terminates its worker and ignores late results',async()=>{
  const h=await phoneScenario({worker:'stall'});const running=h.submit();await h.settled();
  assert.equal(h.workers.length,1);h.el('cancelUpload').handlers.get('click')();await running;await h.settled();
  assert.ok(h.workers[0].terminated);assert.equal(h.starts.length,0);assert.match(h.el('uploadMsg').textContent,/Nothing was uploaded/);
  h.workers[0].onmessage({data:{type:'done',id:h.workers[0].request.id,buffer:Uint8Array.from(h.bundle).buffer}});await h.settled();
  assert.equal(h.starts.length,0);assert.equal(h.timers.size,0);assert.equal(h.el('file').disabled,false);
});

test('preparation has a bounded timeout and an explicit original fallback',async()=>{
  const h=await phoneScenario({worker:'stall'});const running=h.submit();await h.settled();h.expirePreparation();await running;await h.settled();
  assert.ok(h.workers[0].terminated);assert.equal(h.starts.length,0);assert.match(h.el('uploadMsg').textContent,/timed out/);assert.equal(h.el('uploadOriginal').hidden,false);assert.equal(h.timers.size,0);
});

test('failed prepared delivery can retry only the sidecar without uploading the original again',async()=>{
  const options={sidecarFailure:true};const h=await phoneScenario(options);await h.submit();await h.settled();
  assert.deepEqual(h.published.get(h.savedName),h.source);assert.equal(h.sidecars.size,0);assert.equal(h.cancels.length,1);
  assert.equal(h.el('retryPrepared').hidden,false);assert.match(h.el('uploadMsg').textContent,/Uploaded: .*not added/);
  assert.equal(h.el('file').value,'','clear the original selection after its commit so ordinary Upload cannot duplicate it');
  options.sidecarFailure=false;await h.el('retryPrepared').handlers.get('click')();await h.settled();
  assert.equal(h.starts.filter(s=>!s.kind).length,1);assert.equal(h.starts.filter(s=>s.kind==='prepared').length,2);
  assert.deepEqual(h.sidecars.get(h.savedName+'.pdt'),h.bundle);assert.equal(h.published.size,2);assert.equal(h.timers.size,0);
});

test('cancellation cleans only its receiving transaction and preserves existing books',async()=>{
  const h=await phoneScenario({cancelAfterStart:true});h.el('prepareBook').checked=false;await h.submit();await h.settled();
  assert.equal(h.cancels.length,1);assert.equal(h.cancels[0],h.starts[0].requestId);assert.equal(h.published.size,1);assert.equal(h.sidecars.size,0);
});

test('cancellation after original commit reports the saved original and keeps sidecar retry available',async()=>{
  const options={cancelAfterOriginalCommit:true};const h=await phoneScenario(options);await h.submit();await h.settled();
  assert.deepEqual(h.published.get(h.savedName),h.source);assert.equal(h.sidecars.size,0);assert.equal(h.cancels.length,0);
  assert.match(h.el('uploadMsg').textContent,/Uploaded: .*cancelled/);assert.equal(h.el('retryPrepared').hidden,false);
  options.cancelAfterOriginalCommit=false;await h.el('retryPrepared').handlers.get('click')();assert.equal(h.sidecars.size,1);assert.equal(h.starts.filter(s=>!s.kind).length,1);
});

test('oversized EPUB is rejected before worker allocation or HTTP admission',async()=>{
  const h=await phoneScenario();h.file.size=64*1048576+1;await h.submit();
  assert.equal(h.workers.length,0);assert.equal(h.starts.length,0);assert.match(h.el('uploadMsg').textContent,/64 MiB/);
});

const workerHeader = fs.readFileSync(path.join(__dirname, '..', 'src', 'prepare_worker.h'), 'utf8');
const workerScript = workerHeader.match(/R"WORKER\(([\s\S]*?)\)WORKER";/)[1];
async function workerScenario(options = {}) {
  const original = Uint8Array.from([1,2,3,4,5]), expected = new Uint8Array(192).fill(71);
  const messages = [], imports = [], allocations = [], heap = new Uint8Array(4096);
  let reads = 0, releases = 0, prepares = 0, factoryCalls = 0;
  const module = {HEAPU8:heap,
    _pdt_allocate(n){allocations.push(n);return options.oom ? 0 : 32},
    _pdt_prepare(flat){assert.equal(flat,0);assert.deepEqual(heap.slice(32,37),original);++prepares;heap.set(expected,512);return options.importError ? 1 : 0},
    _pdt_result(){return options.badPointer ? -1 : 512},_pdt_result_size(){return options.badSize ? 5e6 : expected.length},
    _pdt_error(){return 99},UTF8ToString(pointer,max){assert.equal(pointer,99);assert.equal(max,256);return 'Unsupported EPUB.'},
    _pdt_release(){++releases;heap.fill(0)},
  };
  const file = {name:'Input.epub',size:original.length,async arrayBuffer(){++reads;if(options.readError)throw new Error('Read failed');return original.slice().buffer}};
  if(options.oversize)file.size=64*1048576+1;
  const context = {self:{postMessage(message,transfers=[]){messages.push({message,transfers})}},Uint8Array,
    importScripts(url){assert.equal(url,'/prepare-core.js');imports.push(url);if(options.assetError)throw new Error('Asset unavailable')},
    async PaperDIYPrepare(config){++factoryCalls;assert.equal(config.locateFile('anything.wasm'),'/prepare-core.wasm');if(options.factoryError)throw new Error('Runtime unavailable');return module},
  };
  vm.createContext(context);vm.runInContext(workerScript,context);
  await context.self.onmessage({data:{type:'prepare',id:'a'.repeat(32),file}});
  return {messages,imports,allocations,reads,releases,prepares,factoryCalls,expected,context};
}

test('production worker copies bounded input to the C++ module and transfers an independent PDT buffer',async()=>{
  const h=await workerScenario();
  assert.equal(h.reads,1);assert.deepEqual(h.allocations,[5]);assert.equal(h.prepares,1);assert.equal(h.releases,1);
  const done=h.messages.find(({message})=>message.type==='done');assert.ok(done);
  assert.deepEqual(new Uint8Array(done.message.buffer),h.expected,'result remains correct after module release overwrites the heap');
  assert.equal(done.transfers.length,1);assert.equal(done.transfers[0],done.message.buffer);
  assert.equal(done.message.sourceName,'Input.epub');assert.equal(h.context.crypto,undefined,'HTTP worker does not need Web Crypto');
  assert.equal(h.messages.filter(({message})=>message.type==='error').length,0);
});
for(const issue of ['oom','readError','importError','badPointer','badSize','assetError','factoryError','oversize']) {
  test(`production worker reports ${issue} without publishing a partial result`,async()=>{
    const h=await workerScenario({[issue]:true});
    assert.equal(h.messages.filter(({message})=>message.type==='done').length,0);
    assert.equal(h.messages.filter(({message})=>message.type==='error').length,1);
    if(issue==='oversize'){assert.equal(h.imports.length,0);assert.equal(h.factoryCalls,0);assert.equal(h.reads,0)}
    else if(!['assetError','factoryError'].includes(issue))assert.equal(h.releases,1);
    if(issue==='oom')assert.equal(h.reads,0);
  });
}

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
}

// Deliberately small chunks exercise binary bytes, multiple offsets and a tail.
const BOOK_BYTES = Buffer.from([0, 1, 2, 253, 254, 255, 3, 4, 5]);
const BOOK_NAME = '中文.epub';
const CHUNK_SIZE = 4;

function jsonResponse(status, body) {
  return { status, ok: status >= 200 && status < 300, async json() { return body; } };
}

class ProtocolFixture {
  constructor(fault) {
    this.fault = fault;
    this.generation = 0;
    this.active = null;
    this.receipt = null;
    this.published = [];
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
        return jsonResponse(200, { state: 'complete', offset: BOOK_BYTES.length, size: BOOK_BYTES.length, name: BOOK_NAME });
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
      assert.equal(args.name, BOOK_NAME);
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
        this.published.push({ name: BOOK_NAME, bytes: this.active.bytes });
        this.active = null;
      }
      assert.equal(args.id, this.receipt.id, 'commit retry must use the same transaction');
      response = jsonResponse(200, { ok: true, name: BOOK_NAME });
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

async function runScenario(fault) {
  const elements = new Map();
  function element(id) {
    if (!elements.has(id)) elements.set(id, new Element());
    return elements.get(id);
  }
  element('file').files = [{
    name: BOOK_NAME,
    size: BOOK_BYTES.length,
    slice(start, end) {
      // ArrayBuffer must cover precisely this slice, without Buffer's pool.
      const bytes = Uint8Array.from(BOOK_BYTES.subarray(start, end));
      return { async arrayBuffer() { return bytes.buffer; } };
    },
  }];
  const fixture = new ProtocolFixture(fault);
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

  assert.equal(fixture.generation, 1, 'recovery must not admit another transaction');
  assert.equal(new Set(fixture.startRequests.map(args => args.requestId)).size, 1);
  assert.equal(element('file').disabled, false, 'release the file control after finishing');
  assert.equal(element('uploadButton').disabled, false, 'release upload after refreshing state');
  assert.equal(timers.size, 0, 'each request must clear its deadline timer');

  if (fault === 'chunk-before') {
    assert.equal(fixture.published.length, 0);
    assert.equal(fixture.active, null);
    assert.equal(fixture.count('/api/upload/chunk'), 3, 'failed chunk retries must be bounded');
    assert.equal(fixture.count('/api/upload/cancel'), 1);
    assert.equal(fixture.count('/api/upload/commit'), 0);
    assert.match(element('uploadMsg').textContent, /Check the book list/);
  } else {
    assert.equal(fixture.published.length, 1);
    assert.deepEqual(fixture.published[0].bytes, BOOK_BYTES);
    assert.deepEqual(fixture.acceptedChunks.map(chunk => chunk.offset), [0, 4, 8]);
    assert.equal(fixture.count('/api/upload/cancel'), 0);
    assert.equal(element('progress').value, 1);
    assert.equal(element('uploadMsg').textContent, 'Uploaded: ' + BOOK_NAME);
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

const projectRoot = require('./setup.cjs');
// Verify the delivered loader and compressed resources, independent of rendering.
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),zlib=require('node:zlib'),crypto=require('node:crypto'),assert=require('node:assert/strict');
const source=fs.readFileSync(path.join(projectRoot,'game.js'),'utf8');
const fallback=vm.runInNewContext('('+source.slice(source.indexOf('function inflateGzip('),source.indexOf('  async function gameBytes()'))+')',{Uint8Array,Uint32Array,DataView,Map,Array,Error});
const wasm=fs.readFileSync(path.join(projectRoot,'game.wasm')),text=fs.readFileSync(path.join(projectRoot,'game-data.js'),'utf8');
const pack=JSON.parse(text.slice('window.MAGIC_TOWER_PACK='.length).trim().slice(0,-1));
assert.deepEqual(zlib.gunzipSync(Buffer.from(pack.data,'base64')),wasm);
assert.deepEqual(Buffer.from(fallback(Buffer.from(pack.data,'base64'),wasm.length)),wasm,'legacy fallback decodes exact shipped WASM');
for(const input of [Buffer.alloc(8),Buffer.alloc(100000,65),crypto.randomBytes(50000),Buffer.from('magic tower '.repeat(30000))]){
 for(const options of [{level:0},{level:9},{strategy:zlib.constants.Z_FIXED}]){
  const gzip=zlib.gzipSync(input,options);assert.deepEqual(Buffer.from(fallback(gzip,input.length)),input);
  const bad=Buffer.from(gzip);bad[bad.length-8]^=1;assert.throws(()=>fallback(bad,input.length));
  assert.throws(()=>fallback(gzip,input.length-1));
 }
}
class Element{
 constructor(id,stages){this.id=id;this.dataset={};this.handlers={};this.hidden=false;this.disabled=true;this.tagName='BUTTON';this.classes=new Set();this.classList={add:c=>this.classes.add(c),remove:c=>this.classes.delete(c),contains:c=>this.classes.has(c)};Object.defineProperty(this,'textContent',{set:v=>{this.text=v;if(id==='loading-stage')stages.push(v);},get:()=>this.text});}
 addEventListener(n,f){this.handlers[n]=f;}
 setAttribute(n,v){this[n]=v;}
 removeAttribute(n){delete this[n];}
 querySelector(){return new Element('child',[]);}
 setPointerCapture(){}
}
async function main(){
 const e=(await WebAssembly.instantiate(wasm)).instance.exports,checks=[];
 for(const mode of ['http','file','fallback','failed','truncated']){
  const stages=[],ids=[...fs.readFileSync(path.join(projectRoot,'index.html'),'utf8').matchAll(/id="([^"]+)"/g)].map(m=>m[1]),els=Object.fromEntries(ids.map(id=>[id,new Element(id,stages)]));
  els['loading-retry'].hidden=true;els.screen.getContext=()=>({createImageData:(w,h)=>({data:new Uint8ClampedArray(w*h*4)}),putImageData(){}});
  const controls=[38,37,39,40,27,41,66].map(k=>{const b=new Element('key-'+k,stages);b.dataset.key=String(k);return b;}),dirs=controls.slice(0,4);
  els.joystick.querySelectorAll=()=>dirs;els.joystick.contains=b=>dirs.includes(b);
  const doc=new Element('document',stages);doc.getElementById=id=>els[id];doc.querySelector=()=>controls[6];doc.querySelectorAll=s=>s==='[data-key]'?controls:s==='.pressed'?[]:[...controls,...Object.values(els)];
  doc.currentScript={dataset:{runtime:'game-data.js?v=test',runtimeSize:String(Buffer.byteLength(text))}};
  const win=new Element('window',stages);let offlineReads=0,requests=0,booted=false;
  win.location={protocol:mode==='file'?'file:':'http:',reload(){}};
  doc.head={appendChild(script){offlineReads++;setTimeout(()=>{win.MAGIC_TOWER_PACK=pack;script.onload();},0);}};doc.createElement=()=>new Element('script',stages);
  const body=new TextEncoder().encode(mode==='truncated'?text.slice(0,-5):text);
  const scope={crypto:require('node:crypto').webcrypto,document:doc,window:win,performance:{now:()=>0},requestAnimationFrame:()=>{booted=true;},localStorage:{getItem:()=>null,setItem(){}},
   Uint8Array,Uint32Array,DataView,Date,Map,Set,JSON,Math,Error,Blob,Response,setTimeout,console,
   DecompressionStream:mode==='fallback'?undefined:DecompressionStream,
   atob:s=>Buffer.from(s,'base64').toString('binary'),btoa:s=>Buffer.from(s,'binary').toString('base64'),
   fetch:async url=>{requests++;assert.equal(url,'game-data.js?v=test');if(mode==='failed')return new Response('',{status:503});let pos=0;const stream=new ReadableStream({pull(controller){if(pos===body.length)return controller.close();const end=Math.min(body.length,pos+123456);controller.enqueue(body.slice(pos,end));pos=end;}});return new Response(stream,{headers:{'Content-Encoding':'gzip','Content-Length':'1'}});},
   WebAssembly:{instantiate:async bytes=>{assert.deepEqual(Buffer.from(bytes),wasm,'all paths initialize exact native runtime');return{instance:{exports:e}};}}};
  vm.runInNewContext(source,scope,{filename:'game.js'});
  for(let n=0;n<500&&!booted&&els['loading-retry'].hidden;n++)await new Promise(resolve=>setTimeout(resolve,10));
  if(mode==='failed'||mode==='truncated'){
   assert.equal(booted,false);assert.equal(els['loading-retry'].hidden,false);assert.equal(els['loading-retry'].disabled,false);assert.equal(els.save.disabled,true);assert.equal(els.loading.hidden,false);
   checks.push(mode+' resource shows failure and retry without enabling game');
  }else{
   assert.ok(booted,mode+' boots');assert.equal(els.loading.hidden,true);assert.equal(els.import.disabled,false);
   assert.ok(stages.includes('解压游戏资源'));assert.ok(stages.includes('启动原版游戏'));assert.ok(stages.includes('读取本地存档'));
   if(mode==='file'){assert.equal(requests,0);assert.equal(offlineReads,1);assert.ok(stages.includes('读取本地游戏资源'));}
   else{assert.equal(offlineReads,0);assert.ok(stages.includes('下载游戏资源 100%'));assert.ok(stages.some(s=>/^下载游戏资源 (?:[1-9]|[1-9][0-9])%$/.test(s)),'progress follows streamed decoded bytes, not gzip Content-Length');}
   checks.push(mode+' loading progress, decompression, native startup and local save check');
  }
 }
 const record={result:'PASS',checks:['exact shipped gzip and WASM equality','stored / fixed / dynamic DEFLATE','CRC corruption and wrong length rejected',...checks],rawWasmBytes:wasm.length,resourceBytes:Buffer.byteLength(text),gzipTransferBytes:zlib.gzipSync(text,{level:9}).length,previousTransferBytes:3650953};
 fs.writeFileSync(path.join(projectRoot,'verification','loading.json'),JSON.stringify(record,null,2));console.log(JSON.stringify(record,null,2));
}
main().catch(err=>{console.error(err);process.exitCode=1;});

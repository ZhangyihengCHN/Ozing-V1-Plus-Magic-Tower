const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const {seal,open:openSave}=require('./test-save-helpers.cjs');
const STORE='hjx-magic-tower-portrait-v1',SLOTS='hjx-magic-tower-portrait-slots-v1',MARK='hjx-magic-tower-save-security-v2';
class Element {
 constructor(id){this.id=id;this.handlers={};this.children=[];this.dataset={};this.hidden=false;this.disabled=false;this.tagName='BUTTON';this.textContent='';this.classes=new Set();this.classList={add:c=>this.classes.add(c),remove:c=>this.classes.delete(c),contains:c=>this.classes.has(c)};}
 matches(){return this.tagName==='BUTTON'&&!this.classList.contains('slot-card');}
 click(){}
 addEventListener(n,f){(this.handlers[n]||=[]).push(f);}
 async fire(n,extra={}){for(const f of this.handlers[n]||[])await f({target:this,preventDefault(){},...extra});}
 append(...children){this.children.push(...children);}
 replaceChildren(...children){this.children=children;}
 setAttribute(n,v){this[n]=v;}
 removeAttribute(n){delete this[n];}
 focus(){if(this.doc)this.doc.activeElement=this;}
 scrollIntoView(){}
 setPointerCapture(){}
 getBoundingClientRect(){return{left:0,top:0,width:144,height:144};}
 getClientRects(){return this.hidden?[]:[{}];}
 querySelector(){return new Element('child');}
 querySelectorAll(){return this.children;}
}
const wasm=fs.readFileSync('game.wasm'),source=fs.readFileSync('game.js','utf8');
async function harness(storage=new Map(),options={}){
 let now=0,frame,core,exported;
 const ids=[...fs.readFileSync('index.html','utf8').matchAll(/id="([^"]+)"/g)].map(m=>m[1]);
 const els=Object.fromEntries(ids.map(id=>[id,new Element(id)]));
 for(const id of ['menu','paused','save-picker','save-overwrite','save-picker-error','save-auto'])els[id].hidden=true;
 const controls=[38,37,39,40,27,41,66].map(k=>{const e=new Element('key-'+k);e.dataset.key=String(k);return e;}),dirs=controls.slice(0,4);
 const doc=new Element('document');doc.getElementById=id=>els[id];doc.createElement=tag=>{const e=new Element(tag);e.tagName=tag.toUpperCase();e.doc=doc;return e;};
 for(const e of Object.values(els))e.doc=doc;
 doc.querySelector=()=>controls[6];doc.querySelectorAll=s=>s==='[data-key]'?controls:s==='.controls > [data-key]'?controls.slice(4,6):s==='.pressed'?controls.filter(e=>e.classList.contains('pressed')):[...controls,...Object.values(els)];
 els.joystick.querySelectorAll=s=>s==='[data-key]'?dirs:dirs.filter(e=>e.classList.contains('pressed'));els.joystick.contains=e=>dirs.includes(e);
 els.screen.getContext=()=>({createImageData:(w,h)=>({data:new Uint8ClampedArray(w*h*4)}),putImageData(){}});
 const win=new Element('window');win.MAGIC_TOWER_WASM=wasm.toString('base64');
 vm.runInNewContext(source,{document:doc,window:win,performance:{now:()=>now},requestAnimationFrame:f=>{frame=f;},
  localStorage:{getItem:key=>{if(options.denyReads)throw new Error('blocked');return storage.get(key)||null;},setItem:(key,value)=>{if(options.failKey===key||options.denyWrites)throw new Error('quota');storage.set(key,value);}},
  crypto:require('node:crypto').webcrypto,Blob,URL:{createObjectURL:b=>{exported=b;return'blob:test';},revokeObjectURL(){}},
  atob:s=>Buffer.from(s,'base64').toString('binary'),btoa:s=>Buffer.from(s,'binary').toString('base64'),
  Uint8Array,Uint32Array,DataView,Date,Map,Set,JSON,Math,Error,setTimeout,console:{error(){},warn(){}},
  WebAssembly:{instantiate:async data=>{const result=await WebAssembly.instantiate(data);core=result.instance.exports;return result;}}},{filename:'game.js'});
 for(let i=0;i<100&&!frame;i++)await new Promise(r=>setTimeout(r,10));assert.ok(frame,'encrypted host booted');
 return{storage,els,core,options,cards:()=>els['save-slots'].children,
  loadAuto:async()=>{await els.continue.fire('click');await els['save-auto'].fire('click');},
  importSave:async(save,name='save.json')=>{const text=typeof save==='string'?save:JSON.stringify(save);await els.import.fire('change',{target:{files:[{name,size:text.length,text:async()=>text}],value:name}});},
  exported:()=>exported,advance:()=>{now+=3000;frame(now);}};
}
async function main(){
 const e=(await WebAssembly.instantiate(wasm)).instance.exports;e.emu_init();e.emu_key(41);e.emu_key(41);e.web_checkpoint();e.emu_save_export(e.web_scratch(),8192);
 const base=Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304));
 const fixture=(hp=1234)=>{const b=Buffer.from(base);b.writeUInt32LE(hp,0x46);return {format:STORE,version:1,savedAt:'2026-10-13T01:00:00.000Z',data:b.toString('base64'),state:{hp:999999}};};
 const legacy=fixture(),oldSlots=[legacy,fixture(2345),fixture(3456),fixture(4567),fixture(5678)],oldArchive=fixture(6789);
 const initial=()=>new Map([[STORE,JSON.stringify(legacy)],[SLOTS,JSON.stringify({format:SLOTS,version:2,slots:oldSlots,archivedSlots:[oldArchive],selected:1})]]);
 const checks=[],h=await harness(initial());
 assert.equal(h.storage.get(MARK),'1');assert.equal(JSON.parse(h.storage.get(STORE)).version,2);
 const wire=JSON.parse(h.storage.get(SLOTS));assert.equal(wire.version,3);assert.equal(wire.slots.length,5);
 for(let i=0;i<5;i++){assert.equal(wire.slots[i].state,undefined);assert.equal(wire.slots[i].savedAt,undefined);assert.equal(openSave(wire.slots[i]).data,oldSlots[i].data);}
 assert.equal(openSave(wire.archivedSlots[0]).data,oldArchive.data);await h.loadAuto();assert.equal(h.core.emu_hp(),1234);
 checks.push('one-time migration preserves autosave, five former slots, archived snapshot, time and exact native bytes without clear metadata');
 const before=new Map(h.storage),liveHp=h.core.emu_hp(),good=JSON.parse(h.storage.get(STORE));
 const flipped=Buffer.from(good.data,'base64');flipped[100]^=1;
 for(const invalid of [{...good,data:flipped.toString('base64')},{...good,nonce:Buffer.alloc(24).toString('base64')},{...good,version:1},legacy]){
  await h.importSave(invalid);assert.equal(h.els['save-picker'].hidden,true);assert.equal(h.core.emu_hp(),liveHp);assert.deepEqual([...h.storage],[...before]);assert.ok(h.els['menu-error'].textContent.includes('无法导入'));
 }
 await h.importSave('unchanged','old.bin');assert.deepEqual([...h.storage],[...before]);
 checks.push('modified ciphertext/nonce, version downgrade, plaintext JSON and BIN imports leave all slots and live progress unchanged');
 const polluted={...good,state:{hp:999999999},savedAt:'2099-01-01T00:00:00Z'};
 await h.importSave(polluted);assert.equal(h.els['save-picker'].hidden,false);await h.els['save-slot-action'].fire('click');await h.els['save-overwrite-confirm'].fire('click');assert.equal(h.core.emu_hp(),1234);
 assert.equal(JSON.parse(h.storage.get(SLOTS)).slots[0].state,undefined);
 checks.push('outer HP/time injection cannot override authenticated binary or persist clear metadata');
 const damaged=JSON.parse(h.storage.get(SLOTS));damaged.slots[1]={...good,data:flipped.toString('base64')};h.storage.set(SLOTS,JSON.stringify(damaged));await h.els.continue.fire('click');assert.equal(h.cards()[1].disabled,true);assert.equal(h.cards()[0].disabled,false);await h.els['save-picker-close'].fire('click');
 checks.push('one damaged encrypted slot does not hide the other valid slots');
 await h.els.export.fire('click');const backup=JSON.parse(await h.exported().text());assert.deepEqual(Object.keys(backup).sort(),['cipher','data','format','nonce','version']);
 const fresh=await harness();await fresh.importSave(backup);await fresh.els['save-slot-action'].fire('click');assert.equal(fresh.core.emu_hp(),1234);
 checks.push('encrypted export/import works in a fresh browser without a password or server');
 for(const failKey of [SLOTS,MARK]){
  const storage=initial(),original=new Map(storage),options={failKey};const failed=await harness(storage,options);
  assert.deepEqual([...storage],[...original]);assert.equal(storage.has(MARK),false);await failed.loadAuto();assert.equal(failed.core.emu_hp(),1234);
  options.failKey=null;await failed.els.save.fire('click');await failed.els['save-slot-action'].fire('click');await failed.els['save-overwrite-confirm'].fire('click');assert.equal(storage.get(MARK),'1');assert.equal(JSON.parse(storage.get(STORE)).version,2);assert.equal(JSON.parse(storage.get(SLOTS)).slots[0].version,2);
 }
 checks.push('migration write/marker failures roll back both original stores, keep old progress readable and retry after storage recovers');
 const after=new Map(h.storage);after.set(STORE,JSON.stringify(fixture(999999)));const reloaded=await harness(after);await reloaded.els.continue.fire('click');assert.equal(reloaded.els['save-auto'].hidden,true);
 checks.push('plaintext localStorage injection after the migration marker is refused on reload');
 await harness(new Map(),{denyReads:true,denyWrites:true});checks.push('blocked browser storage still boots the offline game');
 assert.equal(h.core.emu_last_error(),0);const report={result:'PASS',checks};fs.writeFileSync('verification/save-security.json',JSON.stringify(report,null,2));console.log(JSON.stringify(report,null,2));
}
main().catch(err=>{console.error(err);process.exitCode=1;});

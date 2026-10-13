const projectRoot = require('./setup.cjs');
// Exercise the actual slot UI/storage callbacks with the original WASM runtime.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const {seal,open:openSave}=require('./test-save-helpers.cjs');
const STORE='hjx-magic-tower-portrait-v1',SLOTS='hjx-magic-tower-portrait-slots-v1';
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
async function main(){
 const wasm=fs.readFileSync('game.wasm'),original=(await WebAssembly.instantiate(wasm)).instance.exports;
 original.emu_init();original.emu_key(41);original.emu_key(41);original.web_checkpoint();original.emu_save_export(original.web_scratch(),8192);
 const base=Buffer.from(new Uint8Array(original.memory.buffer,original.web_scratch(),6304));
 const fixture=(floor=1,hp=1234,atk=156,def=178,yellow=2,blue=3,red=4)=>{
  const b=Buffer.from(base);b.fill(0,254);b[5]=floor;b.writeUInt32LE(hp,0x46);b.writeUInt32LE(atk,0x4a);b.writeUInt32LE(def,0x4e);b.writeUInt32LE(yellow,0x5a);b.writeUInt32LE(blue,0x5e);b.writeUInt32LE(red,0x62);b.writeUInt32LE(5,0x76);b.writeUInt32LE(5,0x7a);b.fill(0,254);
  // Keep native player flags and equipment; isolate map movement only.
  base.copy(b,0,0,254);b[5]=floor;for(const [off,n]of[[0x46,hp],[0x4a,atk],[0x4e,def],[0x52,50000+floor],[0x5a,yellow],[0x5e,blue],[0x62,red],[0x76,5],[0x7a,5]])b.writeUInt32LE(n,off);
  return{format:STORE,version:1,savedAt:'2026-10-10T00:01:02.000Z',state:{floor:50,hp:1,atk:1,def:1},data:b.toString('base64')};
 };
 const legacy=fixture(),storage=new Map([[STORE,JSON.stringify(legacy)]]),checks=[];
 let denyWrites=false,now=0,frame,core,exported;
 const ids=[...fs.readFileSync('index.html','utf8').matchAll(/id="([^"]+)"/g)].map(m=>m[1]),els=Object.fromEntries(ids.map(id=>[id,new Element(id)]));
 for(const id of['menu','paused','save-picker','save-overwrite','save-picker-error','save-auto'])els[id].hidden=true;
 const controls=[38,37,39,40,27,41,66].map(k=>{const e=new Element('key-'+k);e.dataset.key=String(k);return e;}),dirs=controls.slice(0,4);
 const doc=new Element('document');doc.tagName='DOCUMENT';doc.getElementById=id=>els[id];doc.createElement=tag=>{const e=new Element(tag);e.tagName=tag.toUpperCase();e.doc=doc;return e;};
 for(const e of Object.values(els))e.doc=doc;
 doc.querySelector=()=>controls[6];doc.querySelectorAll=s=>s==='[data-key]'?controls:s==='.controls > [data-key]'?controls.slice(4,6):s==='.pressed'?controls.filter(e=>e.classList.contains('pressed')):[...controls,...Object.values(els)];
 els.joystick.querySelectorAll=s=>s==='[data-key]'?dirs:dirs.filter(e=>e.classList.contains('pressed'));els.joystick.contains=e=>dirs.includes(e);
 els.screen.getContext=()=>({createImageData:(w,h)=>({data:new Uint8ClampedArray(w*h*4)}),putImageData(){}});
 const win=new Element('window');win.MAGIC_TOWER_WASM=wasm.toString('base64');
 vm.runInNewContext(fs.readFileSync('game.js','utf8'),{crypto:require('node:crypto').webcrypto,document:doc,window:win,performance:{now:()=>now},requestAnimationFrame:f=>{frame=f;},localStorage:{getItem:k=>storage.get(k)||null,setItem:(k,v)=>{if(denyWrites)throw new Error('quota');storage.set(k,v);}},
  Blob,URL:{createObjectURL:b=>{exported=b;return 'blob:private-test';},revokeObjectURL(){}},
  atob:s=>Buffer.from(s,'base64').toString('binary'),btoa:s=>Buffer.from(s,'binary').toString('base64'),Uint8Array,Uint32Array,DataView,Date,Map,Set,JSON,Math,Error,setTimeout,console,
  WebAssembly:{instantiate:async data=>{const r=await WebAssembly.instantiate(data);core=r.instance.exports;return r;}}},{filename:'game.js'});
 for(let i=0;i<100&&!frame;i++)await new Promise(r=>setTimeout(r,10));assert.ok(frame);
 const wireSlots=()=>JSON.parse(storage.get(SLOTS));
 const slots=()=>{const d=wireSlots();return {...d,slots:d.slots.map(openSave),archivedSlots:(d.archivedSlots||[]).map(openSave)};},cards=()=>els['save-slots'].children;
 assert.equal(slots().slots.length,3);assert.equal(slots().slots[0].data,legacy.data);assert.equal(slots().slots[0].state.floor,1);assert.equal(slots().slots[0].state.hp,1234);assert.equal(openSave(JSON.parse(storage.get(STORE))).data,legacy.data);assert.equal(JSON.parse(storage.get(STORE)).version,2);assert.equal(JSON.parse(storage.get(STORE)).state,undefined);checks.push('legacy migration without deleting or trusting metadata');
 await els.continue.fire('click');assert.equal(cards().length,3);assert.equal(cards()[1].disabled,true);await cards()[0].fire('click');await els['save-slot-action'].fire('click');assert.equal(core.emu_hp(),1234);assert.equal(core.emu_floor(),1);checks.push('title opens three slots and empty slots are disabled for reading');
 const autoRead=async save=>{storage.set(STORE,JSON.stringify(seal(save)));await els.continue.fire('click');await els['save-auto'].fire('click');};
 const expected=[];
 for(let i=0;i<3;i++){
  const save=fixture([1,4,12,32,46][i],2000+i*500,200+i*10,150+i*20,10+i,20+i,30+i);fs.writeFileSync('verification/slot-fixture-'+(i+1)+'.json',JSON.stringify(save));await autoRead(save);await els.save.fire('click');assert.equal(els['save-picker'].hidden,false);await cards()[i].fire('click');await els['save-slot-action'].fire('click');
  if(i===0){assert.equal(els['save-overwrite'].hidden,false);assert.equal(slots().slots[0].data,legacy.data);await els['save-overwrite-confirm'].fire('click');}
  assert.equal(els['save-picker'].hidden,true);expected.push(slots().slots[i]);
  assert.deepEqual(slots().slots[i].state,{floor:[1,4,12,32,46][i],x:5,y:5,hp:2000+i*500,atk:200+i*10,def:150+i*20,gold:50000+[1,4,12,32,46][i],yellow:10+i,blue:20+i,red:30+i});
 }
 checks.push('three independent native snapshots with exact HP attack defense floor and key counts');
 const extra=[fixture(32,4567),fixture(46,8765)];storage.set(SLOTS,JSON.stringify({...wireSlots(),slots:[...wireSlots().slots,...extra],selected:4}));
 await autoRead(expected[0]);await els.save.fire('click');assert.equal(cards().length,3);await cards()[0].fire('click');await els['save-slot-action'].fire('click');await els['save-overwrite-confirm'].fire('click');
 assert.equal(slots().slots.length,3);assert.deepEqual(slots().archivedSlots.map(s=>s.data),extra.map(s=>s.data));
 checks.push('old fourth/fifth snapshots are preserved in storage when writing the three visible slots');
 const snapshot=JSON.stringify(slots().slots);await autoRead(fixture(1,9999));await doc.fire('keydown',{key:'ArrowRight',repeat:false});await doc.fire('keyup',{key:'ArrowRight'});now=3000;frame(now);assert.equal(JSON.stringify(slots().slots),snapshot);checks.push('automatic checkpoint never overwrites manual slots');
 for(let i=0;i<3;i++){
  await els.continue.fire('click');const row=cards()[i],stats=row.children[1];assert.equal(stats.children.length,7);assert.equal(stats.children[4]['aria-label'],'黄钥匙 '+(10+i));assert.equal(stats.children[5]['aria-label'],'蓝钥匙 '+(20+i));assert.equal(stats.children[6]['aria-label'],'红钥匙 '+(30+i));assert.equal(stats.children[3]['aria-label'],'金币 '+(50000+[1,4,12,32,46][i]));assert.ok(row.children[2].textContent.includes('2026'));
  await row.fire('click');await els['save-slot-action'].fire('click');assert.equal(core.emu_floor(),expected[i].state.floor);assert.equal(core.emu_hp(),expected[i].state.hp);assert.equal(core.emu_atk(),expected[i].state.atk);assert.equal(core.emu_def(),expected[i].state.def);assert.equal(JSON.stringify(slots().slots),snapshot);
 }
 checks.push('each selected slot restores original stats and preserves all snapshots');
 await els.save.fire('click');await cards()[0].fire('click');await els['save-slot-action'].fire('click');await els['save-overwrite-cancel'].fire('click');assert.equal(JSON.stringify(slots().slots),snapshot);await els['save-picker-close'].fire('click');assert.equal(JSON.stringify(slots().slots),snapshot);checks.push('overwrite cancellation and closing do not write snapshots');
 await els.save.fire('click');const position=[core.emu_x(),core.emu_y()];await doc.fire('keydown',{key:'ArrowUp',repeat:false});now+=5000;frame(now);assert.deepEqual([core.emu_x(),core.emu_y()],position);await doc.fire('keydown',{key:'Escape'});assert.equal(els['save-picker'].hidden,true);checks.push('picker pauses timers and keyboard movement; Escape closes');
 await els.save.fire('click');await cards()[0].fire('click');await els['save-slot-action'].fire('click');denyWrites=true;await els['save-overwrite-confirm'].fire('click');assert.equal(els['save-picker'].hidden,false);assert.equal(els['save-picker-error'].hidden,false);assert.equal(JSON.stringify(slots().slots),snapshot);await els['save-picker-close'].fire('click');
 await els.continue.fire('click');await cards()[0].fire('click');await els['save-slot-action'].fire('click');assert.equal(core.emu_floor(),1);denyWrites=false;checks.push('storage failure retains picker and saved data; reading still works without write access');
 // Real screen controls navigate the picker without reaching the firmware.
 storage.set(SLOTS,JSON.stringify({...wireSlots(),selected:0}));
 await els.continue.fire('click');const before=[core.emu_floor(),core.emu_x(),core.emu_y(),core.emu_hp()];
 assert.equal(dirs.some(b=>b.disabled),false);assert.equal(controls[5].disabled,false);
 await controls[5].fire('pointerdown',{pointerId:1});await controls[5].fire('pointerup');assert.equal(els['save-picker'].hidden,true);
 await els.continue.fire('click');await doc.fire('keydown',{key:'ArrowDown',repeat:false});await doc.fire('keyup',{key:'ArrowDown'});
 assert.equal(cards()[1].classList.contains('selected'),true);assert.deepEqual([core.emu_floor(),core.emu_x(),core.emu_y(),core.emu_hp()],before);
 await doc.fire('keydown',{key:'Enter',repeat:false});assert.equal(core.emu_floor(),4);assert.equal(els['save-picker'].hidden,true);
 await els.save.fire('click');await doc.fire('keydown',{key:'Enter',repeat:false});assert.equal(els['save-overwrite'].hidden,false);
 await doc.fire('keydown',{key:'Enter',repeat:false});assert.equal(els['save-overwrite'].hidden,true);assert.equal(JSON.stringify(slots().slots),snapshot);
 await doc.fire('keydown',{key:'Enter',repeat:false});await doc.fire('keydown',{key:'ArrowRight',repeat:false});await doc.fire('keyup',{key:'ArrowRight'});
 await doc.fire('keydown',{key:'Enter',repeat:false});assert.equal(els['save-picker'].hidden,true);assert.equal(slots().slots[1].state.floor,4);
 checks.push('joystick/keyboard selection, hardware confirm, safe default overwrite cancellation and direction confirmation');
 await autoRead(expected[0]);
 const broken=wireSlots();broken.slots[2]={format:STORE,data:'corrupt'};storage.set(SLOTS,JSON.stringify(broken));await win.fire('storage',{key:SLOTS});await els.continue.fire('click');assert.equal(cards()[2].disabled,true);assert.equal(cards()[1].disabled,false);await els['save-picker-close'].fire('click');checks.push('one damaged slot does not hide the other valid slots');
 const imported=fixture(12,7777);els.import.files=[{size:9000,name:'import.json',text:async()=>JSON.stringify(seal(imported))}];await els.import.fire('change');assert.equal(els['save-picker'].hidden,false);assert.equal(core.emu_floor(),1);await cards()[2].fire('click');await els['save-slot-action'].fire('click');assert.equal(core.emu_floor(),12);assert.equal(core.emu_hp(),7777);assert.equal(slots().slots[2].state.floor,12);checks.push('import selects destination before loading and derives original binary metadata');
 const wake=JSON.parse(fs.readFileSync('verification/prison-wake-fixture.json','utf8'));
 await autoRead(wake);await els.save.fire('click');await cards()[2].fire('click');await els['save-slot-action'].fire('click');await els['save-overwrite-confirm'].fire('click');
 const savedWake=slots().slots[2];assert.equal(savedWake.testRewardClaimed,undefined);
 assert.equal(savedWake.state.gold,4);for(const key of ['yellow','blue','red'])assert.equal(savedWake.state[key],0);
 const wakeBytes=Buffer.from(savedWake.data,'base64');assert.equal(wakeBytes.readUInt32LE(0x3d),0);assert.equal(wakeBytes.readUInt32LE(0x41),0);
 for(let i=0;i<10;i++){now+=3000;frame(now);}
 await autoRead({...wake,testRewardClaimed:false});await els.save.fire('click');await cards()[2].fire('click');await els['save-slot-action'].fire('click');await els['save-overwrite-confirm'].fire('click');
 assert.equal(slots().slots[2].data,savedWake.data);assert.equal(slots().slots[2].testRewardClaimed,undefined);
 checks.push('wake-up import, idle frames, save and reload keep original inventory and coins without any test gifts');
 // Old development saves remain readable; obsolete metadata is discarded, progress is preserved.
 const oldDevelopment=Buffer.from(wake.data,'base64');for(const [off,value]of[[0x3d,0x7fff],[0x41,999],[0x52,99999],[0x5a,999],[0x5e,999],[0x62,999]])oldDevelopment.writeUInt32LE(value,off);
 await autoRead({...wake,testRewardClaimed:true,data:oldDevelopment.toString('base64')});await els.save.fire('click');await cards()[2].fire('click');await els['save-slot-action'].fire('click');await els['save-overwrite-confirm'].fire('click');
 const restoredDevelopment=Buffer.from(oldDevelopment);restoredDevelopment.writeUInt32LE(3,0x41);
 assert.equal(slots().slots[2].data,restoredDevelopment.toString('base64'));assert.equal(slots().slots[2].testRewardClaimed,undefined);
 checks.push('legacy999 symmetric flights restore to3 while other binary progress and old save compatibility are preserved');
 for(const remaining of [0,1,2,3]){
  const used=Buffer.from(restoredDevelopment);used.writeUInt32LE(remaining,0x41);if(!remaining)used.writeUInt32LE(used.readUInt32LE(0x3d)&~0x100,0x3d);
  await autoRead({...wake,data:used.toString('base64')});assert.equal(core.web_item_count(6),remaining);
 }
 checks.push('loading charges0 through3 never refills already consumed symmetric flights');
 await controls[4].fire('pointerdown',{pointerId:3});await controls[4].fire('pointerup');await els['menu-title'].fire('click');
 await doc.fire('keydown',{key:'Enter',repeat:false});await doc.fire('keydown',{key:'Enter',repeat:false});
 assert.equal(core.emu_floor(),1);assert.equal(core.emu_hp(),1000);await els.save.fire('click');await cards()[0].fire('click');await els['save-slot-action'].fire('click');await els['save-overwrite-confirm'].fire('click');
 assert.equal(slots().slots[0].testRewardClaimed,undefined);assert.equal(slots().slots[0].state.gold,0);assert.equal(slots().slots[0].state.yellow,0);
 checks.push('new adventures keep original starting inventory and omit test gift metadata');
 // Regression: a focused Close button must never interpret Enter as Load.
 await autoRead(fixture(1,4321));await els.continue.fire('click');const unchanged=[core.emu_floor(),core.emu_x(),core.emu_y(),core.emu_hp()];
 await doc.fire('keydown',{key:'Enter',target:els['save-picker-close']});assert.equal(els['save-picker'].hidden,false);assert.deepEqual([core.emu_floor(),core.emu_x(),core.emu_y(),core.emu_hp()],unchanged);
 await els['save-picker-close'].fire('click');assert.equal(els['save-picker'].hidden,true);
 checks.push('focused Close keeps native keyboard activation and cannot accidentally load a slot');
 // Failed auto storage must retry and export the live progress, not an older save.
 await doc.fire('keydown',{key:'ArrowRight',repeat:false});await doc.fire('keyup',{key:'ArrowRight'});const moved=core.emu_x(),oldAuto=storage.get(STORE);denyWrites=true;now+=3000;frame(now);assert.equal(storage.get(STORE),oldAuto);
 await controls[4].fire('click',{detail:0});await els['menu-title'].fire('click');assert.equal(core.emu_mode(),0);assert.equal(core.emu_x(),moved);assert.equal(els['menu-error'].hidden,false,'return to title must not discard unpersisted progress');
 await els.export.fire('click');const backupWire=JSON.parse(await exported.text()),backup=openSave(backupWire);assert.equal(backup.state.x,moved);assert.equal(backup.state.hp,4321,'export uses the live state even while storage refuses writes');
 await controls[4].fire('click',{detail:0});assert.equal(els.menu.hidden,true,'controller Return resumes the menu');denyWrites=false;now+=3000;frame(now);assert.equal(openSave(JSON.parse(storage.get(STORE))).state.x,moved);
 checks.push('auto-save retries after quota failures, title return preserves unsaved progress, and export captures the live game');
 await controls[4].fire('click',{detail:0});assert.equal(els.menu.hidden,false);await controls[5].fire('click',{detail:0});assert.equal(els.menu.hidden,true,'controller Confirm resumes the menu');
 checks.push('menu controls can resume with Return or Confirm');
 // Damaged multi-byte fields are refused before imports can touch the live core.
 const keptSlots=storage.get(SLOTS),keptAuto=storage.get(STORE),corrupt=Buffer.from(backup.data,'base64');corrupt.writeUInt32LE(257,5);
 await els.import.fire('change',{target:{files:[{name:'bad.json',size:9000,text:async()=>JSON.stringify({...backupWire,data:corrupt.toString('base64')})}],value:'bad.json'}});
 assert.equal(els['save-picker'].hidden,true);assert.equal(storage.get(SLOTS),keptSlots);assert.equal(storage.get(STORE),keptAuto);assert.equal(core.emu_x(),moved);assert.equal(els['menu-error'].hidden,false);assert.ok(els['menu-error'].textContent.includes('无法导入'));
 checks.push('invalid 32-bit floor import leaves live progress and every stored slot untouched');
 const basement=JSON.parse(fs.readFileSync('verification/basement-fixture.json','utf8'));
 await autoRead(basement);assert.equal(core.emu_floor(),0);await els.save.fire('click');await cards()[2].fire('click');await els['save-slot-action'].fire('click');await els['save-overwrite-confirm'].fire('click');
 assert.equal(slots().slots[2].state.floor,0);assert.equal(slots().slots[2].data,basement.data);
 await els.continue.fire('click');assert.ok(cards()[2].children[0].children[1].children[1].textContent.includes('第 0 层'));await cards()[2].fire('click');await els['save-slot-action'].fire('click');assert.equal(core.emu_floor(),0);
 els.import.files=[{size:9000,name:'basement.json',text:async()=>JSON.stringify(seal(basement))}];await els.import.fire('change');assert.equal(els['save-picker'].hidden,false);await els['save-picker-close'].fire('click');
 checks.push('legal basement floor zero supports automatic restore, manual save/load, slot metadata and JSON import');
 assert.equal(core.emu_last_error(),0);fs.writeFileSync('verification/save-slots.json',JSON.stringify({result:'PASS',checks},null,2));console.log(JSON.stringify({result:'PASS',checks},null,2));
}
main().catch(e=>{console.error(e);process.exitCode=1;});

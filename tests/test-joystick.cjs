const projectRoot = require('./setup.cjs');
// Exercise the shipped input code against the real game core with a minimal DOM.
// The empty first-floor test fixture isolates input from walls and encounters.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const {seal}=require('./test-save-helpers.cjs');
const STORE = 'hjx-magic-tower-portrait-v1';
class Element {
  constructor(id) {
    this.id=id; this.handlers={}; this.hidden=false; this.disabled=false;
    this.textContent=''; this.dataset={}; this.tagName='BUTTON';
    this.children=[];
    const classes=new Set(),styles=new Map();
    this.classList={add:c=>classes.add(c),remove:c=>classes.delete(c),contains:c=>classes.has(c)};
    this.style={setProperty:(k,v)=>styles.set(k,v),removeProperty:k=>styles.delete(k),getPropertyValue:k=>styles.get(k)||''};
  }
  addEventListener(name,cb) { (this.handlers[name]||=[]).push(cb); }
  fire(name,values={}) { for(const cb of this.handlers[name]||[]) cb({target:this,preventDefault(){},...values}); }
  setPointerCapture() {}
  setAttribute(name,value) { this[name]=value; }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children=children; }
  focus() { if(this.doc)this.doc.activeElement=this; }
  click() { if(!this.disabled)this.fire("click",{detail:0}); }
  removeAttribute(name) { delete this[name]; }
  getBoundingClientRect() { return {left:0,top:0,width:144,height:144}; }
}
async function main() {
  const wasm=fs.readFileSync(path.join(projectRoot,'game.wasm'));
  const {instance}=await WebAssembly.instantiate(wasm),e=instance.exports;
  e.emu_init(); e.emu_key(41); e.emu_key(41); e.web_checkpoint();
  e.emu_save_export(e.web_scratch(),8192);
  const bytes=Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304));
  bytes.fill(0,254,254+121); bytes.writeUInt32LE(5,0x76); bytes.writeUInt32LE(5,0x7a);
  const fixture={format:STORE,version:1,data:bytes.toString('base64')};
  const ids=Array.from(fs.readFileSync(path.join(projectRoot,'index.html'),'utf8').matchAll(/id="([^"]+)"/g),m=>m[1]);
  const els=Object.fromEntries(ids.map(id=>[id,new Element(id)]));
  for(const id of ['menu','paused','save-picker','save-overwrite','save-picker-error','save-auto'])els[id].hidden=true;
  const controls=[38,37,39,40,27,41,66].map(key=>{const el=new Element('key-'+key);el.dataset.key=String(key);return el;});
  const pad=els.joystick,dirs=controls.slice(0,4);
  pad.contains=el=>dirs.includes(el);
  pad.querySelectorAll=selector=>selector==='[data-key]'?dirs:dirs.filter(el=>el.classList.contains('pressed'));
  pad.querySelector=selector=>dirs.find(el=>selector.includes('"'+el.dataset.key+'"'));
  const drawnFrames=new Set();
  els.screen.getContext=()=>({createImageData:()=>({data:new Uint8ClampedArray(240*320*4)}),
    putImageData(pixels){drawnFrames.add(require('node:crypto').createHash('sha256').update(pixels.data).digest('hex'));}});
  els.loading.querySelector=()=>new Element('loading-child');
  const doc=new Element('document'); doc.hidden=false;
  doc.getElementById=id=>els[id];
  for(const el of Object.values(els))el.doc=doc;
  els.menu.querySelectorAll=()=>['menu-resume','menu-save','menu-title','export'].map(id=>els[id]).filter(el=>!el.disabled);
  els.screen.tagName='CANVAS';
  doc.querySelector=selector=>selector==='[data-key="66"]'?controls.find(el=>el.dataset.key==='66'):new Element('details');
  doc.querySelectorAll=selector=>selector==='[data-key]'?controls:selector==='.controls > [data-key]'?controls.filter(el=>['27','41'].includes(el.dataset.key)):selector==='.pressed'?controls.filter(el=>el.classList.contains('pressed')):[...controls,...Object.values(els)];
  doc.createElement=tag=>{const el=new Element(tag);el.tagName=tag.toUpperCase();return el;};
  const storage=new Map(),win=new Element('window');
  let now=0,frame,core;
  const scope={crypto:require('node:crypto').webcrypto,document:doc,window:win,performance:{now:()=>now},requestAnimationFrame:cb=>{frame=cb;},
    localStorage:{getItem:k=>storage.get(k)||null,setItem:(k,v)=>storage.set(k,v)},
    atob:s=>Buffer.from(s,'base64').toString('binary'),btoa:s=>Buffer.from(s,'binary').toString('base64'),
    Uint8Array,Uint32Array,DataView,Date,Map,Set,JSON,Math,Error,setTimeout,
    WebAssembly:{instantiate:async data=>{const r=await WebAssembly.instantiate(data);core=r.instance.exports;return r;}}};
  win.MAGIC_TOWER_WASM=wasm.toString('base64');
  vm.runInNewContext(fs.readFileSync(path.join(projectRoot,'game.js'),'utf8'),scope,{filename:'game.js'});
  for(let n=0;n<100&&!frame;n++) await new Promise(resolve=>setTimeout(resolve,10));
  assert.ok(frame,'game finished booting');
  const bookButton=controls.find(el=>el.dataset.key==='66');
  assert.equal(els.save.disabled,true,'saving is unavailable on the title screen');
  assert.equal(bookButton.disabled,true,'book is unavailable on the title screen');
  assert.equal(els.continue.disabled,true,'reading is unavailable without a stored save');
  assert.equal(els.export.disabled,true,'export is unavailable on title without a stored save');
  assert.equal(els.import.disabled,false,'import is available after boot');
  for(let i=0;i<2;i++){doc.fire('keydown',{key:'Enter',repeat:false});doc.fire('keyup',{key:'Enter'});}
  assert.equal(els.save.disabled,false,'new game enables saving');
  assert.equal(els.export.disabled,false,'new game can export its current progress');
  assert.equal(els.continue.disabled,true,'new game has not yet been saved');
  els.save.fire('click');els['save-slots'].children[0].fire('click');els['save-slot-action'].fire('click');assert.equal(els.continue.disabled,false,'successful save enables reading');
  controls.find(el=>el.dataset.key==='27').fire('click',{detail:0});els['menu-title'].fire('click');
  assert.equal(els.save.disabled,true,'returning to title disables saving');
  assert.equal(els.continue.disabled,false,'stored progress can be read from title');
  assert.equal(els.export.disabled,false,'stored progress can be exported from title');
  const readAuto=()=>{els.continue.fire('click');els['save-auto'].fire('click');};
  const reset=()=>{storage.set(STORE,JSON.stringify(seal(fixture)));readAuto();assert.deepEqual([core.emu_x(),core.emu_y()],[5,5]);};
  const pos=()=>[core.emu_x(),core.emu_y()];
  const advance=ms=>{const stop=now+ms;while(now<stop){now+=16;frame(now);}};
  const pointer=(name,x,y,id=1)=>pad.fire(name,{clientX:x,clientY:y,pointerId:id,isPrimary:true});
  const checks=[];
  reset();assert.equal(els.save.disabled,false,'saving is available on an idle map');
  assert.equal(bookButton.disabled,true,'unowned book is unavailable');
  checks.push('toolbar availability on title and idle map');
  for(const [key,code,axis,sign] of [['ArrowUp','ArrowUp',1,-1],['w','KeyW',1,-1],['a','KeyA',0,-1],['s','KeyS',1,1],['d','KeyD',0,1],['Unidentified','KeyW',1,-1]]){
    reset();doc.fire('keydown',{key,code,repeat:false});assert.equal(pos()[axis],5+sign);assert.equal(doc.activeElement,els.screen);
    assert.equal(pad.querySelectorAll('.pressed').length,1);const first=pos();doc.fire('keydown',{key,code,repeat:true});assert.deepEqual(pos(),first);
    advance(450);assert.ok((pos()[axis]-5)*sign>=2);doc.fire('keyup',{key,code});const stopped=pos();advance(450);assert.deepEqual(pos(),stopped);assert.equal(pad.querySelectorAll('.pressed').length,0);
  }
  reset();for(const extra of [{ctrlKey:true},{altKey:true},{metaKey:true},{target:{tagName:'INPUT'}},{target:{isContentEditable:true}}]){doc.fire('keydown',{key:'w',code:'KeyW',repeat:false,...extra});assert.deepEqual(pos(),[5,5]);}
  doc.fire('keydown',{key:'x',code:'KeyX',repeat:false});assert.equal(els.menu.hidden,false);assert.equal(doc.activeElement,els['menu-resume']);
  doc.fire('keydown',{key:'ArrowDown',repeat:false});assert.equal(doc.activeElement,els['menu-save']);doc.fire('keydown',{key:'z',code:'KeyZ',repeat:false});assert.equal(els['save-picker'].hidden,false);
  doc.fire('keydown',{key:'x',code:'KeyX',repeat:false});assert.equal(els['save-picker'].hidden,true);doc.fire('keydown',{key:'Escape',repeat:false});assert.equal(els.menu.hidden,true);assert.equal(doc.activeElement,els.screen);
  doc.fire('keydown',{key:'F5',repeat:false});assert.equal(els['save-picker'].hidden,false);doc.fire('keydown',{key:'Escape',repeat:false});assert.equal(els['save-picker'].hidden,true);
  doc.fire('keydown',{key:'F9',repeat:false});assert.equal(els['save-picker'].hidden,false);doc.fire('keydown',{key:'Escape',repeat:false});assert.equal(els['save-picker'].hidden,true);
  checks.push('arrows and physical WASD with hold/release/highlight, ignored modifiers and editable fields, keyboard menu navigation, Z/X confirm/return, F5 save and F9 load');
  for(const [name,x,y,axis,sign] of [['up',72,20,1,-1],['down',72,124,1,1],['left',20,72,0,-1],['right',124,72,0,1]]) {
    reset();pointer('pointerdown',x,y);assert.equal(pos()[axis],5+sign,'first step '+name);
    assert.deepEqual(pad.querySelectorAll('.pressed').map(el=>el.dataset.key),[String({up:38,down:40,left:37,right:39}[name])]);
    advance(650);assert.ok((pos()[axis]-5)*sign>=3,'hold repeats '+name);
    pointer('pointerup',x,y);const stopped=pos();advance(600);assert.deepEqual(pos(),stopped,'release stops '+name);
    assert.equal(pad.querySelectorAll('.pressed').length,0);
    checks.push('hold and release '+name);
  }
  reset();pointer('pointerdown',124,72);advance(350);const right=pos();
  pointer('pointermove',72,124);advance(350);assert.equal(pos()[0],right[0]);assert.ok(pos()[1]>=7);
  assert.deepEqual(pad.querySelectorAll('.pressed').map(el=>el.dataset.key),['40']);
  pointer('pointermove',72,72);const centered=pos();advance(600);assert.deepEqual(pos(),centered);
  assert.equal(pad.querySelectorAll('.pressed').length,0);
  pointer('pointerup',72,72);checks.push('slide changes direction and center stops');
  for(const [name,x,y] of [['center',72,72],['near center',91,72],['outside circle',140,140],['outside pad',170,72]]) {
    reset();pointer('pointerdown',124,72);pointer('pointermove',x,y);const stopped=pos();advance(600);
    assert.deepEqual(pos(),stopped);assert.equal(pad.querySelectorAll('.pressed').length,0);
    pointer('pointerup',x,y);checks.push(name+' clears highlight and stops');
  }
  for(const [name,x,y,key,axis,sign] of [['upper diagonal',120,20,38,1,-1],['right near center',95,72,39,0,1],['lower diagonal',120,124,40,1,1],['left diagonal',20,40,37,0,-1]]) {
    reset();pointer('pointerdown',x,y);assert.equal(pos()[axis],5+sign);
    assert.deepEqual(pad.querySelectorAll('.pressed').map(el=>el.dataset.key),[String(key)]);
    pointer('pointerup',x,y);checks.push(name+' activates entire sector');
  }
  for(const name of ['pointercancel','lostpointercapture']) {
    reset();pointer('pointerdown',20,72);pointer(name,20,72);const stopped=pos();advance(600);assert.deepEqual(pos(),stopped);checks.push(name+' stops');
  }
  reset();pointer('pointerdown',124,72);win.fire('blur');const blurred=pos();advance(600);assert.deepEqual(pos(),blurred);checks.push('window blur stops');
  reset();pointer('pointerdown',124,72);controls.find(el=>el.dataset.key==='27').fire('click',{detail:0});const paused=pos();advance(600);assert.deepEqual(pos(),paused);
  pointer('pointerdown',72,20);advance(500);assert.deepEqual(pos(),paused);els['menu-resume'].fire('click');advance(500);assert.deepEqual(pos(),paused);checks.push('menu and resume do not leave movement held');
  pad.getBoundingClientRect=()=>({left:0,top:0,width:196,height:196});
  reset();pointer('pointerdown',170,98);pointer('pointermove',124,98);const largeCentered=pos();advance(600);assert.deepEqual(pos(),largeCentered);assert.equal(pad.querySelectorAll('.pressed').length,0);
  pointer('pointermove',130,98);assert.equal(pos()[0],largeCentered[0]+1);pointer('pointerup',130,98);checks.push('enlarged pad scales its neutral center and preserves direction input');
  // Run the actual browser frame loop against a private multi-hit encounter.
  const encounterBytes=Buffer.from(bytes);encounterBytes[254+4*11+5]=0xc3;
  const encounter={...fixture,data:encounterBytes.toString('base64')};
  pad.getBoundingClientRect=()=>({left:0,top:0,width:144,height:144});
  const resetEncounter=()=>{storage.set(STORE,JSON.stringify(seal(encounter)));readAuto();drawnFrames.clear();};
  const finishBattle=()=>{for(let i=0;i<150&&core.web_combat_active();i++)advance(16);assert.equal(core.web_combat_active(),0);};
  resetEncounter();
  pointer('pointerdown',72,20);assert.equal(core.web_combat_active(),1);
  assert.equal(els.save.disabled,true,'save dims during combat');
  assert.equal(bookButton.disabled,true,'book dims during combat');
  assert.equal(els.export.disabled,false,'existing save can still be exported during combat');
  assert.equal(els.continue.disabled,false,'existing save can still be loaded during combat');
  assert.equal(els.import.disabled,false,'a save can still be imported during combat');
  assert.deepEqual(pad.querySelectorAll('.pressed').map(el=>el.dataset.key),['38'],'combat keeps the held direction');
  advance(350);assert.deepEqual(pos(),[5,4]);finishBattle();assert.equal(core.emu_hp(),160);
  assert.equal(els.save.disabled,false,'save becomes available after combat');
  assert.deepEqual(pos(),[5,4],'movement is suspended until combat completes');
  assert.ok(drawnFrames.size>=3,'browser publishes distinct original combat frames');
  advance(350);assert.ok(pos()[1]<4,'held direction continues after victory');
  pointer('pointerup',72,20);const resumedStop=pos();advance(500);assert.deepEqual(pos(),resumedStop);
  checks.push('original combat animation with continuous held movement after victory');
  for(const stop of ['pointerup','pointercancel','lostpointercapture','center','blur']){
    resetEncounter();pointer('pointerdown',72,20);advance(350);assert.equal(core.web_combat_active(),1);
    if(stop==='center')pointer('pointermove',72,72);
    else if(stop==='blur')win.fire('blur');
    else pointer(stop,72,20);
    finishBattle();advance(500);assert.deepEqual(pos(),[5,4],stop+' during combat cancels resumed movement');
    assert.equal(pad.querySelectorAll('.pressed').length,0);
    pointer('pointerup',72,72);checks.push(stop+' during combat prevents automatic movement');
  }
  resetEncounter();pointer('pointerdown',72,20);advance(350);pointer('pointermove',124,72);
  assert.deepEqual(pos(),[5,4],'sliding cannot move the hero during combat');
  assert.deepEqual(pad.querySelectorAll('.pressed').map(el=>el.dataset.key),['39']);
  finishBattle();advance(300);assert.ok(pos()[0]>5);assert.equal(pos()[1],4);
  pointer('pointerup',124,72);checks.push('slide during combat changes the direction used after victory');
  resetEncounter();doc.fire('keydown',{key:'ArrowUp',repeat:false});advance(350);finishBattle();advance(300);
  assert.ok(pos()[1]<4,'keyboard hold continues after victory');
  doc.fire('keyup',{key:'ArrowUp'});const keyboardStop=pos();advance(400);assert.deepEqual(pos(),keyboardStop);
  resetEncounter();doc.fire('keydown',{key:'ArrowUp',repeat:false});advance(350);doc.fire('keyup',{key:'ArrowUp'});
  finishBattle();advance(400);assert.deepEqual(pos(),[5,4]);
  checks.push('keyboard hold resumes after victory and release during combat stops it');
  const easyBytes=Buffer.from(bytes);easyBytes[254+4*11+5]=0xc0;easyBytes[254+3*11+5]=0xc0;
  storage.set(STORE,JSON.stringify(seal({...fixture,data:easyBytes.toString('base64')})));readAuto();
  pointer('pointerdown',72,20);advance(750);pointer('pointerup',72,20);
  assert.equal(core.web_combat_active(),0);assert.ok(pos()[1]<3,'one hold crosses successive one-hit encounters');
  assert.equal(core.emu_hp(),1000);assert.equal(core.web_checkpoint(),1);core.emu_save_export(core.web_scratch(),8192);
  assert.equal(new DataView(core.memory.buffer).getUint32(core.web_scratch()+0x52,true),2,'both monsters award gold once');
  checks.push('continuous hold through consecutive one-hit encounters');
  const bookBytes=Buffer.from(bytes);bookBytes.writeUInt32LE(bookBytes.readUInt32LE(0x3d)|1,0x3d);
  for(const expanded of [0,1]){
    bookBytes.writeUInt32LE(expanded,0x21);
    storage.set(STORE,JSON.stringify(seal({...fixture,data:bookBytes.toString('base64')})));readAuto();
    assert.equal(bookButton.disabled,false,'owned book is available on the map');
    bookButton.fire('pointerdown',{pointerId:7});bookButton.fire('pointerup',{pointerId:7});bookButton.fire('click',{detail:1});
    assert.equal(core.emu_dialog_active(),1,'one physical tap opens the book without an extra click');
    assert.equal(bookButton.disabled,false,'book remains enabled for closing its own dialog');assert.equal(bookButton['aria-pressed'],'true');
    assert.equal(els.save.disabled,true,'save dims while a native dialog is open');
    bookButton.fire('pointerdown',{pointerId:7});bookButton.fire('pointerup',{pointerId:7});bookButton.fire('click',{detail:1});assert.equal(core.emu_dialog_active(),0,'second physical tap closes the original book');assert.equal(bookButton['aria-pressed'],'false');
    doc.fire('keydown',{key:'b',repeat:false});doc.fire('keyup',{key:'b'});
    assert.equal(core.emu_dialog_active(),1,'B opens book with one key press');
    doc.fire('keydown',{key:'b',repeat:false});doc.fire('keyup',{key:'b'});assert.equal(core.emu_dialog_active(),0,'B also toggles the book closed');
    doc.fire('keydown',{key:'b',repeat:false});doc.fire('keyup',{key:'b'});doc.fire('keydown',{key:'Escape',repeat:false});doc.fire('keyup',{key:'Escape'});assert.equal(core.emu_dialog_active(),0);
  }
  checks.push('single book button tap opens/closes, B toggles, and Escape still closes from either inventory view');
  const notebook=JSON.parse(fs.readFileSync('verification/notebook-fixture.json','utf8'));
  storage.set(STORE,JSON.stringify(seal(notebook)));readAuto();
  doc.fire('keydown',{key:'Unidentified',code:'KeyN',repeat:false});doc.fire('keyup',{key:'Unidentified',code:'KeyN'});assert.equal(core.emu_dialog_active(),1);
  doc.fire('keydown',{key:'ArrowDown',repeat:false});doc.fire('keyup',{key:'ArrowDown'});assert.equal(core.emu_dialog_active(),1,'direction selects a notebook entry without closing');
  doc.fire('keydown',{key:'z',code:'KeyZ',repeat:false});doc.fire('keyup',{key:'z',code:'KeyZ'});assert.equal(core.emu_dialog_active(),1,'Z reads selected notebook record');
  doc.fire('keydown',{key:'Enter',repeat:false});doc.fire('keyup',{key:'Enter'});doc.fire('keydown',{key:'Escape',repeat:false});doc.fire('keyup',{key:'Escape'});assert.equal(core.emu_dialog_active(),0);assert.equal(els.save.disabled,false);
  checks.push('physical N opens original notebook, directions select, Z reads, and Enter/Return restore playable map');
  for(let slot=0;slot<10;slot++){
    const itemBytes=Buffer.from(bytes);itemBytes.writeUInt32LE(1<<(slot+2),0x3d);itemBytes.writeUInt32LE(3,0x41);itemBytes.writeUInt32LE(3,9);itemBytes[254+59]=6;if(slot===1)itemBytes[254+49]=1;
    storage.set(STORE,JSON.stringify(seal({...fixture,data:itemBytes.toString('base64')})));readAuto();
    assert.equal(els['quick-'+slot].disabled,false);assert.equal(els['quick-icon-'+slot].hidden,false);
    for(let other=0;other<10;other++)if(other!==slot){assert.equal(els['quick-'+other].disabled,true);assert.equal(els['quick-icon-'+other].hidden,true);}
    const digit=String((slot+1)%10),code=(slot%2?'Numpad':'Digit')+digit;
    doc.fire('keydown',{key:digit,code,repeat:false});doc.fire('keyup',{key:digit,code});
    if(slot===0){assert.equal(core.emu_dialog_active(),1);doc.fire('keydown',{key:'Escape',repeat:false});doc.fire('keyup',{key:'Escape'});}
    else if(slot===1)assert.equal(core.web_item_owned(slot),1);
    else if(slot===6){assert.equal(core.web_item_count(slot),2);doc.fire('keydown',{key:digit,code,repeat:true});assert.equal(core.web_item_count(slot),2);assert.equal(els['quick-count-'+slot].textContent,'2');}
    else{assert.equal(core.web_item_owned(slot),0);assert.equal(els['quick-icon-'+slot].hidden,true);assert.equal(els['quick-'+slot].disabled,true);}
    if(slot===5)assert.equal(core.emu_hp(),1200);if(slot===7)assert.equal(core.emu_floor(),2);if(slot===8)assert.equal(core.emu_floor(),0);
  }
  const pickupItem=Buffer.from(bytes);pickupItem[254+49]=0x3b;storage.set(STORE,JSON.stringify(seal({...fixture,data:pickupItem.toString('base64')})));readAuto();assert.equal(els['quick-icon-5'].hidden,true);
  doc.fire('keydown',{key:'ArrowUp',repeat:false});doc.fire('keyup',{key:'ArrowUp'});assert.equal(core.web_item_owned(5),1);assert.equal(els['quick-icon-5'].hidden,false,'real native pickup immediately fills the matching slot');
  els['quick-5'].fire('click');assert.equal(core.emu_hp(),1200);assert.equal(els['quick-icon-5'].hidden,true);
  checks.push('all10 digit/numpad bindings and item icons track real ownership, consume once, reject key repeat, update charges and support touch/click');
  // Shortcut eligibility and a physical hold released by a floor jump.
  const flight=JSON.parse(fs.readFileSync('verification/flight-fixture.json','utf8'));
  storage.set(STORE,JSON.stringify(seal(flight)));readAuto();assert.equal(els['fly-up'].disabled,false);assert.equal(els['fly-down'].disabled,false);
  doc.fire('keydown',{key:'PageDown',repeat:false});assert.equal(core.emu_floor(),2);assert.equal(core.emu_dialog_active(),0);assert.deepEqual(pos(),[0,9]);
  doc.fire('keydown',{key:'PageUp',repeat:false});assert.equal(core.emu_floor(),3);assert.equal(core.emu_dialog_active(),0);
  storage.set(STORE,JSON.stringify(seal(flight)));readAuto();
  // Up is blocked by a wall here, so holding it leaves the stair eligibility intact.
  pointer('pointerdown',72,20);const near=[core.emu_x(),core.emu_y()];assert.deepEqual(near,[1,10]);els['fly-up'].fire('click');assert.equal(core.emu_floor(),4);const landed=pos();advance(1000);assert.deepEqual(pos(),landed,'flight releases the held direction');pointer('pointerup',72,20);
  controls.find(el=>el.dataset.key==='27').fire('click',{detail:0});assert.equal(els['fly-up'].disabled,true);assert.equal(els['fly-down'].disabled,true);controls.find(el=>el.dataset.key==='41').fire('click',{detail:0});
  const beforeRepeat=core.emu_floor();doc.fire('keydown',{key:'PageDown',repeat:true});assert.equal(core.emu_floor(),beforeRepeat);
  checks.push('toolbar and PageUp/PageDown flight switch floors directly, reject key repeat, dim while paused, and clear joystick hold');
  checks.push('toolbar follows combat, ownership, dialog, and return to map');
  assert.equal(core.emu_last_error(),0);
  const report={result:'PASS',checks};
  fs.writeFileSync(path.join(projectRoot,'verification','joystick.json'),JSON.stringify(report,null,2)+'\n');
  console.log(JSON.stringify(report,null,2));
}
main().catch(err=>{console.error(err);process.exitCode=1;});

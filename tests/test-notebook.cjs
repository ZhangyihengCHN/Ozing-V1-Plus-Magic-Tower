const projectRoot = require('./setup.cjs');
const fs=require('node:fs'),assert=require('node:assert/strict'),crypto=require('node:crypto');
(async()=>{
 const e=(await WebAssembly.instantiate(fs.readFileSync('game.wasm'))).instance.exports;
 const check=()=>assert.equal(e.emu_last_error(),0,e.emu_last_error_pc().toString(16));
 const key=k=>{assert.equal(e.emu_key(k),1);e.emu_paint();check();};
 const touch=(x,y)=>{assert.equal(e.web_touch(x,y),1);e.emu_paint();check();};
 const save=()=>{assert.equal(e.web_checkpoint(),1);assert.equal(e.emu_save_export(e.web_scratch(),8192),1);return Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304));};
 const pixels=()=>new Uint32Array(e.memory.buffer,e.emu_framebuffer(),240*320);
 const hash=()=>crypto.createHash('sha256').update(new Uint8Array(e.memory.buffer,e.emu_framebuffer(),240*320*4)).digest('hex');
 e.emu_init();key(41);key(41);const base=save();
 const load=(records=0,view=0,owned=true)=>{const b=Buffer.from(base);b.writeUInt32LE(owned?2:0,0x3d);b.writeUInt32LE(records,0x39);b.writeUInt32LE(view,0x21);e.emu_init();new Uint8Array(e.memory.buffer,e.web_scratch(),6304).set(b);assert.equal(e.emu_save_import(e.web_scratch(),6304),1);key(40);key(41);return b;};
 const exit=()=>{for(let i=0;i<8&&e.emu_dialog_active();i++)key(27);assert.equal(e.emu_dialog_active(),0);assert.equal(e.web_can_checkpoint(),1);};
 load(0,0,false);key(78);assert.equal(e.emu_dialog_active(),0);
 load();key(78);assert.equal(e.emu_dialog_active(),1);assert.equal(pixels()[61*240+41],0xffffff);key(38);assert.equal(e.emu_dialog_active(),1,'empty notebook direction does not dismiss it');key(41);assert.equal(e.emu_dialog_active(),0);save();
 for(const view of [0,1]){
  load(0x3fffff,view);key(78);assert.equal(e.emu_dialog_active(),1);assert.equal(e.web_checkpoint(),0);assert.equal(pixels()[61*240+41],0xffffff,'opaque original notebook background');
  const grid=hash();for(let i=0;i<30;i++){e.emu_paint();assert.equal(hash(),grid);}
  key(40);assert.notEqual(hash(),grid,'direction visibly selects a record');assert.equal(e.emu_dialog_active(),1);
  key(41);assert.equal(e.emu_dialog_active(),1,'confirm opens original record text');key(41);exit();
  const after=save();assert.equal(after.readUInt32LE(0x39),0x3fffff);assert.equal(after.readUInt32LE(0x3d),2);assert.equal(e.emu_hp(),1000);
 }
 load(0x3fffff);touch(127,265);assert.equal(e.emu_dialog_active(),1,'inventory icon opens notebook in one tap');
 const initial=hash();touch(55,69);assert.equal(e.emu_dialog_active(),1);assert.notEqual(hash(),initial,'tapping a record opens text instead of closing the notebook');key(41);key(27);assert.equal(e.emu_dialog_active(),0);save();
 load((1<<3)|(1<<11));key(78);key(39);key(41);assert.equal(e.emu_dialog_active(),1,'keyboard skips unavailable records');key(41);exit();save();
 for(let record=0;record<22;record++){
  load(0x3fffff);key(78);touch(57+(record%5)*31,70+Math.floor(record/5)*17);assert.equal(e.emu_dialog_active(),1);key(41);exit();const b=save();assert.equal(b.readUInt32LE(0x39),0x3fffff);assert.equal(e.emu_hp(),1000);check();
 }
 load(1);const original=save();
 for(let i=0;i<100;i++){key(78);touch(55,69);key(41);exit();const b=save();assert.deepEqual(b.subarray(0x39),original.subarray(0x39),'reading preserves inventory stats story and maps');}
 // Loading while a record dialog is open must discard both old continuations.
 key(78);touch(55,69);load(1);key(78);key(27);assert.equal(e.emu_dialog_active(),0);save();
 const fixture=load(0x3fffff);fs.writeFileSync('verification/notebook-fixture.json',JSON.stringify({format:'hjx-magic-tower-portrait-v1',version:1,savedAt:'2026-10-12T00:00:00Z',data:fixture.toString('base64')}));
 const report={result:'PASS',checks:['ownership check and empty notebook','opaque original grid background and stable redraws','directions select available records and confirm reads original text','inventory icon opens and record taps read instead of dismissing','all22 original records open and exit without corrupting gameplay','100 nested notebook/dialog/return/save cycles preserve inventory stats story and maps','loading while reading safely clears old nested continuations']};
 fs.writeFileSync('verification/notebook.json',JSON.stringify(report,null,2));console.log(JSON.stringify(report,null,2));
})().catch(err=>{console.error(err);process.exitCode=1;});

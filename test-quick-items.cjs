const fs=require('node:fs'),assert=require('node:assert/strict'),crypto=require('node:crypto');
(async()=>{
 const e=(await WebAssembly.instantiate(fs.readFileSync('game.wasm'))).instance.exports;
 const check=()=>assert.equal(e.emu_last_error(),0,e.emu_last_error_pc().toString(16));
 const save=()=>{assert.equal(e.web_checkpoint(),1);assert.equal(e.emu_save_export(e.web_scratch(),8192),1);check();return Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304));};
 const load=b=>{assert.equal(e.emu_init(),1);new Uint8Array(e.memory.buffer,e.web_scratch(),6304).set(b);assert.equal(e.emu_save_import(e.web_scratch(),6304),1);e.emu_key(40);e.emu_key(41);e.emu_paint();check();};
 const hash=()=>crypto.createHash('sha256').update(new Uint8Array(e.memory.buffer,e.emu_framebuffer(),240*320*4)).digest('hex');
 e.emu_init();for(let i=0;i<10;i++){assert.equal(e.web_item_owned(i),0);assert.equal(e.web_can_item(i),0);assert.equal(e.web_item_use(i),0);}e.emu_key(41);e.emu_key(41);const base=save();
 const fixture=(slot=0)=>{const b=Buffer.from(base);b.writeUInt32LE(3,5);b.writeUInt32LE(5,9);b.writeUInt32LE(5,0x76);b.writeUInt32LE(5,0x7a);b.writeUInt32LE(5,0x7e);b.writeUInt32LE(5,0x82);b.writeUInt32LE(0,0x21);b.writeUInt32LE(1<<(slot+2),0x3d);b.writeUInt32LE(3,0x41);b.fill(0,254+2*121,254+3*121);for(const [index,tile]of[[59,6],[61,2],[49,1],[71,0xc0]])b[254+2*121+index]=tile;return b;};
 for(let slot=0;slot<10;slot++){
  const b=fixture(slot);load(b);const before=save();assert.equal(e.web_item_owned(slot),1);assert.equal(e.web_can_item(slot),1);
  for(let other=0;other<10;other++)if(other!==slot){assert.equal(e.web_item_owned(other),0);assert.equal(e.web_item_use(other),0);}
  assert.equal(e.web_item_use(10),0);assert.equal(e.web_item_use(0xffffffff),0);assert.deepEqual(save(),before);
  for(let i=0;i<100;i++){e.web_item_owned(slot);e.web_item_count(slot);e.web_can_item(slot);}assert.deepEqual(save(),before,'availability is read-only');
  assert.equal(e.web_item_use(slot),1);check();const quickPixels=hash();let quick;
  if(slot===0){assert.equal(e.emu_dialog_active(),1);assert.equal(e.web_can_item(slot),0);assert.equal(e.web_item_use(slot),0);e.emu_key(27);quick=save();}
  else quick=save();
  load(b);const item=slot+2,x=92+(item%6)*25+10,y=item<6?265:290;assert.equal(e.web_touch(x,y),1);e.emu_paint();check();assert.equal(hash(),quickPixels,'original icon and shortcut render identically for item '+item);
  if(slot===0)e.emu_key(27);assert.deepEqual(save(),quick,'original icon and shortcut produce identical snapshots for item '+item);
  if(slot===1){assert.equal(e.web_item_owned(slot),1);assert.equal(e.web_can_item(slot),0,'ice dims after all lava is cleared');}
  if(slot===5){assert.equal(e.emu_hp(),1200);assert.equal(e.web_item_owned(slot),0);}
  if(slot===6){assert.equal(e.web_item_count(slot),2);assert.deepEqual([e.emu_x(),e.emu_y()],[5,5]);}
 }
 const away=fixture(0);away.writeUInt32LE(1,0x76);away.writeUInt32LE(1,0x7a);load(away);assert.equal(e.web_item_owned(0),1);assert.equal(e.web_can_item(0),0);
 const blocked=fixture(6);blocked[254+2*121+60]=2;load(blocked);assert.equal(e.web_item_owned(6),1);assert.equal(e.web_can_item(6),0);assert.equal(e.web_item_use(6),0);assert.equal(e.web_item_count(6),3);
 load(fixture(6));for(let i=0;i<3;i++)assert.equal(e.web_item_use(6),1);assert.equal(e.web_item_owned(6),0);assert.equal(e.web_item_count(6),0);assert.equal(e.web_item_use(6),0);
 const combat=fixture(5);combat[254+2*121+49]=0xc3;load(combat);e.emu_key(38);assert.equal(e.web_combat_active(),1);assert.equal(e.web_item_owned(5),1);assert.equal(e.web_can_item(5),0);assert.equal(e.web_item_use(5),0);check();
 // Original descending item on floor1 retains the valid basement route.
 const basement=Buffer.from(base);basement.writeUInt32LE(0x400,0x3d);basement.writeUInt32LE(1,0x76);basement.writeUInt32LE(0,0x7a);load(basement);assert.equal(e.web_item_use(8),1);assert.equal(e.emu_floor(),0);save();
 const all=fixture();all.writeUInt32LE(0xffc,0x3d);all.writeUInt32LE(2,5);all.writeUInt32LE(3,9);fs.writeFileSync('verification/quick-items-fixture.json',JSON.stringify({format:'hjx-magic-tower-portrait-v1',version:1,savedAt:'2026-10-12T00:00:00Z',data:all.toString('base64')}));
 const report={result:'PASS',checks:['all10 shortcuts exactly match original icon frames and save snapshots','ownership and idle/menu/combat guards','read-only ownership/count/availability queries','consumable removal and remaining flight counts','floor-flight staircase eligibility and original popup','first-floor descending item reaches basement','no test items are granted by the shortcut bar']};fs.writeFileSync('verification/quick-items.json',JSON.stringify(report,null,2));console.log(JSON.stringify(report,null,2));
})().catch(err=>{console.error(err);process.exitCode=1;});

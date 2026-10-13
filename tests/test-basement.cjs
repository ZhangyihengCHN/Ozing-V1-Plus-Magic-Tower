const projectRoot = require('./setup.cjs');
const fs=require('node:fs'),assert=require('node:assert/strict');
(async()=>{
 const e=(await WebAssembly.instantiate(fs.readFileSync('game.wasm'))).instance.exports;
 const check=()=>assert.equal(e.emu_last_error(),0,e.emu_last_error_pc().toString(16));
 const key=k=>{assert.equal(e.emu_key(k),1);e.emu_paint();check();};
 const save=()=>{assert.equal(e.web_checkpoint(),1);assert.equal(e.emu_save_export(e.web_scratch(),8192),1);check();return Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304));};
 const load=b=>{assert.equal(e.emu_init(),1);new Uint8Array(e.memory.buffer,e.web_scratch(),6304).set(b);assert.equal(e.emu_save_import(e.web_scratch(),6304),1);key(40);key(41);};
 // Use the firmware's real descending-item key and original first-floor map.
 e.emu_init();key(41);key(41);const start=save();
 start.writeUInt32LE(0x404,0x3d);start.writeUInt32LE(1,0x76);start.writeUInt32LE(0,0x7a);
 load(start);key(68);assert.equal(e.emu_floor(),0);assert.deepEqual([e.emu_x(),e.emu_y()],[1,0]);
 const basement=save();assert.equal(basement.readUInt32LE(0x3d),4,'descending item consumed once');
 assert.deepEqual(basement.subarray(254),start.subarray(254),'basement saving preserves all 50 maps');
 fs.writeFileSync('verification/basement-fixture.json',JSON.stringify({format:'hjx-magic-tower-portrait-v1',version:1,savedAt:'2026-10-12T00:00:00Z',data:basement.toString('base64')}));
 // Pixel and snapshot identity proves a reload reconstructs the original room.
 const pixels=()=>Buffer.from(new Uint8Array(e.memory.buffer,e.emu_framebuffer(),240*320*4));
 e.emu_paint();const before=pixels();load(basement);assert.deepEqual(pixels(),before);assert.deepEqual(save(),basement);
 assert.equal(e.web_can_fly(0),1,'can fly back to first floor beside basement stairs');assert.equal(e.web_can_fly(1),0);
 assert.equal(e.web_fly(0),1);assert.equal(e.emu_floor(),1);assert.deepEqual(save().subarray(254),start.subarray(254));
 load(basement);for(let i=0;i<4;i++)key(39);for(let i=0;i<5;i++)key(40);
 const acquired=save();assert.equal(acquired.readUInt32LE(0x3d)&0x4000,0x4000,'original gold-doubling item acquired');
 assert.deepEqual(acquired.subarray(254),start.subarray(254));
 key(40);const cleanAcquired=save(),acquiredPixels=pixels();load(cleanAcquired);assert.deepEqual(pixels(),acquiredPixels,'collected item does not reappear after loading');assert.deepEqual(save(),cleanAcquired);
 // Walking across the old pickup cell cannot duplicate the item.
 key(40);key(38);key(40);const once=save();assert.equal(once.readUInt32LE(0x3d),acquired.readUInt32LE(0x3d));
 assert.equal(once.readUInt32LE(0x52),acquired.readUInt32LE(0x52));
 e.web_tick(100);const idleAcquired=save();
 for(let i=0;i<100;i++){
  load(idleAcquired);e.web_tick(100);const saved=save(),expected=Buffer.from(idleAcquired);
  // The original idle event updates its previous-position bookkeeping.
  saved.copy(expected,0x7e,0x7e,0x86);assert.deepEqual(saved,expected);
 }
 // Return via the original staircase and verify actual doubled battle gold.
 load(acquired);while(e.emu_y()>0)key(38);while(e.emu_x()>1)key(37);key(37);assert.equal(e.emu_floor(),1);
 const returned=save();assert.deepEqual(returned.subarray(254),start.subarray(254));
 const fight=Buffer.from(returned);fight.fill(0,254,254+121);fight[254+4*11+5]=0xc0;fight.writeUInt32LE(5,0x76);fight.writeUInt32LE(5,0x7a);
 load(fight);const gold=fight.readUInt32LE(0x52);key(38);for(let n=0;n<20&&e.web_combat_active();n++)e.emu_timer(2);
 assert.equal(save().readUInt32LE(0x52),gold+2,'original green slime gold doubles from1 to2');
 const report={result:'PASS',checks:['real first-floor descending item reaches floor zero and is consumed','original basement map restored byte-for-byte after loading','saving and leaving floor zero preserve all 50 regular maps','gold-doubling item acquired with original pickup logic','100 basement load/save cycles retain inventory and do not respawn pickup','original staircase and flyer return to first floor','battle gold doubles with the acquired original item']};
 fs.writeFileSync('verification/basement.json',JSON.stringify(report,null,2));console.log(JSON.stringify(report,null,2));
})().catch(err=>{console.error(err);process.exitCode=1;});

const projectRoot = require('./setup.cjs');
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
async function main() {
  const {instance} = await WebAssembly.instantiate(fs.readFileSync(path.join(projectRoot,'game.wasm')));
  const e = instance.exports;
  assert.equal(e.web_test_reward,undefined,"test gift export has been removed");
  assert.deepEqual(WebAssembly.Module.imports(new WebAssembly.Module(fs.readFileSync(path.join(projectRoot,'game.wasm')))),[]);
  const state = () => ({floor:e.emu_floor(),x:e.emu_x(),y:e.emu_y(),hp:e.emu_hp(),atk:e.emu_atk(),def:e.emu_def()});
  const check = () => assert.equal(e.emu_last_error(),0,`ARM error at ${e.emu_last_error_pc().toString(16)}`);
  const key = code => {
    assert.equal(e.emu_key(code),1); e.emu_paint(); check();
    for(let n=0;n<100&&!e.web_busy()&&!e.emu_dialog_active();n++) {
      for(let i=0;i<16;i++) { const id=e.web_timer_id(i);if(!id)break; const ms=e.web_timer_interval(i)*5;if((n*5)%ms===0)e.emu_timer(id); }
    }
    e.emu_paint();check();
  };
  const tick = ms => { assert.equal(e.web_tick(ms),1); check(); };
  assert.equal(e.emu_init(),1); check();
  assert.equal(e.emu_mode(),1);
  assert.equal(e.web_touch(120,105),1);
  assert.equal(e.web_timer_id(0),5);
  assert.equal(e.web_timer_interval(0),40);
  key(41); assert.equal(e.emu_mode(),0);
  assert.deepEqual(state(),{floor:1,x:5,y:10,hp:1000,atk:100,def:100});
  key(38); assert.equal(e.emu_y(),9);
  key(37); assert.equal(e.emu_x(),5); // Item pickup leaves the hero in place.
  assert.equal(e.web_checkpoint(),1); check();
  const savedState=state();
  assert.equal(e.emu_save_size(),6304);
  assert.equal(e.emu_save_export(e.web_scratch(),8192),1);
  const save=new Uint8Array(e.memory.buffer,e.web_scratch(),6304).slice();
  assert.equal(e.emu_init(),1);
  new Uint8Array(e.memory.buffer,e.web_scratch(),6304).set(save);
  assert.equal(e.emu_save_import(e.web_scratch(),6303),0);
  assert.equal(e.emu_save_import(e.web_scratch(),6304),1);
  key(40); key(41); assert.deepEqual(state(),savedState);
  for (const [floor,x,y] of [[1,5,7],[1,10,7],[1,10,0],[1,0,0],[2,0,10],[3,4,10],[3,4,8]]) {
    for (let n=0;n<40;n++) {
      const s=state();
      if(s.floor!==floor || s.x===x&&s.y===y) break;
      key(s.x!==x ? (s.x<x?39:37) : (s.y<y?40:38));
      if(e.web_busy()) break;
      if(e.emu_dialog_active()) break;
      assert.notEqual(n,39,'route stuck '+JSON.stringify(state()));
    }
  }
  assert.equal(e.emu_floor(),3);
  assert.equal(e.web_busy(),1000,'original pre-capture delay');
  tick(999); assert.equal(e.emu_dialog_active(),0);
  tick(1); assert.equal(e.emu_dialog_active(),1);
  for(let n=0;n<8 && e.emu_floor()===3;n++) key(41);
  assert.equal(e.emu_floor(),2);
  assert.equal(e.emu_hp(),400);
  assert.equal(e.web_busy(),4000,'original unconscious delay is 800 x 5 ms');
  const prison=state(); key(38); assert.deepEqual(state(),prison,'movement blocked while unconscious');
  tick(3999); assert.equal(e.emu_dialog_active(),0);
  tick(1); assert.equal(e.emu_dialog_active(),1);
  key(41); assert.equal(e.emu_dialog_active(),0);
  assert.deepEqual(state(),{floor:2,x:3,y:7,hp:400,atk:10,def:10});
  assert.equal(e.web_checkpoint(),1); check();
  assert.equal(e.emu_save_export(e.web_scratch(),8192),1);
  fs.writeFileSync("verification/prison-wake-fixture.json",JSON.stringify({format:"hjx-magic-tower-portrait-v1",version:1,data:Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304)).toString("base64")}));
  const wake=Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304));
  const inventory=()=>{
    assert.equal(e.web_checkpoint(),1);assert.equal(e.emu_save_export(e.web_scratch(),8192),1);
    const bytes=Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304));
    assert.equal(bytes.readUInt32LE(0x3d),0,'no gifted items');
    assert.equal(bytes.readUInt32LE(0x41),0,'no gifted consumables');
    for(const off of [0x5a,0x5e,0x62])assert.equal(bytes.readUInt32LE(off),0,'no gifted keys');
    assert.equal(bytes.readUInt32LE(0x52),4,'only original route coins remain');
    assert.deepEqual(state(),{floor:2,x:3,y:7,hp:400,atk:10,def:10});check();
  };
  inventory();assert.equal(e.web_can_book(),0);assert.equal(e.web_can_fly(0),0);assert.equal(e.web_can_fly(1),0);
  for(let n=0;n<100;n++){
    e.emu_paint();e.web_tick(100);inventory();
    assert.equal(e.emu_init(),1);new Uint8Array(e.memory.buffer,e.web_scratch(),6304).set(wake);
    assert.equal(e.emu_save_import(e.web_scratch(),6304),1);e.emu_key(40);e.emu_key(41);inventory();
  }
  const report={result:'PASS',checks:['test gift export removed','original capture delays and wake dialogue unchanged','wake HP400 ATK10 DEF10 unchanged','no gifted items consumables or keys','original route coins4 preserved','unowned monster book and flight stay unavailable','100 idle repaint and save/load cycles never grant anything']};
  fs.writeFileSync('verification/prison-wake.json',JSON.stringify(report,null,2));console.log(JSON.stringify(report,null,2));
}
main().catch(error=>{console.error(error);process.exitCode=1;});

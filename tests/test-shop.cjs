const projectRoot = require('./setup.cjs');
// Private fixtures exercise the original firmware economy, never player saves.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
async function main() {
  const {instance} = await WebAssembly.instantiate(fs.readFileSync(path.join(projectRoot,'game.wasm')));
  const e = instance.exports;
  const check = () => assert.equal(e.emu_last_error(),0,`ARM error ${e.emu_last_error_pc().toString(16)}`);
  const key = k => {assert.equal(e.emu_key(k),1);e.emu_paint();check();};
  const touch = (x,y) => {assert.equal(e.web_touch(x,y),1);e.emu_paint();check();};
  const save = () => {
    assert.equal(e.web_checkpoint(),1,'game must remain playable and saveable');
    assert.equal(e.emu_save_export(e.web_scratch(),8192),1);
    return Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304));
  };
  e.emu_init();touch(120,105);key(41);
  assert.equal(e.emu_mode(),0);
  const base = save();
  const positions = {4:[5,1],12:[5,9],32:[9,10],46:[5,1]};
  const fixture = (floor,gold=10000) => {
    const b=Buffer.from(base);b[5]=floor;
    b.writeUInt32LE(positions[floor][0],0x76);b.writeUInt32LE(positions[floor][1],0x7a);
    b.writeUInt32LE(gold,0x52);return b;
  };
  const load = b => {
    assert.equal(e.emu_init(),1);
    new Uint8Array(e.memory.buffer,e.web_scratch(),b.length).set(b);
    assert.equal(e.emu_save_import(e.web_scratch(),b.length),1);key(40);key(41);
    assert.equal(e.emu_mode(),0);assert.equal(e.emu_dialog_active(),0);check();
  };
  const open = () => {key(38);assert.equal(e.emu_dialog_active(),1,'bump shop opens upgrade window');};
  for(const [floor,attack,defense] of [[4,2,4],[12,4,8],[32,8,16],[46,10,20]]) {
    load(fixture(floor));
    const before={hp:e.emu_hp(),atk:e.emu_atk(),def:e.emu_def()};
    open();touch(120,108);assert.equal(e.emu_dialog_active(),0);
    assert.equal(e.emu_hp(),before.hp+100);let b=save();assert.equal(b.readUInt32LE(0x52),9980);
    assert.equal(b.readUInt32LE(0x35),40);
    open();key(40);key(41);assert.equal(e.emu_atk(),before.atk+attack);
    b=save();assert.equal(b.readUInt32LE(0x52),9940);assert.equal(b.readUInt32LE(0x35),80);
    open();touch(120,158);assert.equal(e.emu_def(),before.def+defense);
    b=save();assert.equal(b.readUInt32LE(0x52),9860);assert.equal(b.readUInt32LE(0x35),140);
    open();key(27);assert.equal(e.emu_dialog_active(),0);assert.deepEqual(save(),b);
    open();key(38);key(41);assert.equal(e.emu_dialog_active(),0);assert.deepEqual(save(),b);
    load(b);assert.equal(e.emu_hp(),before.hp+100);assert.equal(e.emu_atk(),before.atk+attack);
    assert.equal(e.emu_def(),before.def+defense);
    open();touch(120,108);assert.equal(e.emu_hp(),before.hp+500,'health gain grows with total purchases');
    b=save();assert.equal(b.readUInt32LE(0x52),9720);
  }
  load(fixture(4,0));const before=save();open();touch(120,133);
  assert.equal(e.emu_atk(),100);assert.equal(e.emu_dialog_active(),1,'insufficient gold opens original dialogue');
  for(let i=0;i<8&&e.emu_dialog_active();i++)touch(120,200);
  assert.equal(e.emu_dialog_active(),0);
  const after=save();
  assert.deepEqual(after.subarray(0x35,0x39),before.subarray(0x35,0x39));
  assert.deepEqual(after.subarray(0x46,0x56),before.subarray(0x46,0x56));
  open();touch(120,183);key(40);assert.equal(e.emu_y(),2,'can move after cancelling');
  load(fixture(4,1000000));
  for(let i=0;i<100;i++){open();key(27);save();}
  const checks=['four original shop floors','tap health / attack / defense / leave',
    'direction selection and confirmation','original price increases and gold deductions',
    'upgrades survive save / load','insufficient gold dialogue','movement after closing','100 reopen / cancel / save cycles',
    'original firmware background colour','stable popup redraw without Web selection overlay'];
  fs.writeFileSync(path.join(projectRoot,'verification/shop-fixture.json'),JSON.stringify({format:'hjx-magic-tower-portrait-v1',version:1,data:fixture(4).toString('base64')}));
  load(fixture(4));open();
  const pixels=new Uint32Array(e.memory.buffer,e.emu_framebuffer(),240*320);
  assert.equal(pixels[45*240+45],0xebebeb,'firmware WM_ERASEBKGND uses its original system colour');
  const original = new Uint32Array(pixels);
  e.emu_paint();check();assert.deepEqual(pixels,original,'shop redraw remains stable');
  key(40);assert.deepEqual(pixels,original,'direction selection adds no Web decoration');
  const rgba=Buffer.alloc(pixels.length*4);
  for(let i=0;i<pixels.length;i++){
    rgba[i*4]=pixels[i]>>>16;rgba[i*4+1]=pixels[i]>>>8;rgba[i*4+2]=pixels[i];rgba[i*4+3]=255;
  }
  fs.writeFileSync(path.join(projectRoot,'verification/shop-fixed.rgba'),rgba);
  fs.writeFileSync(path.join(projectRoot,'verification/shop.json'),JSON.stringify({result:'PASS',checks},null,2)+'\n');
  console.log(JSON.stringify({result:'PASS',checks},null,2));
}
main().catch(error=>{console.error(error);process.exitCode=1;});

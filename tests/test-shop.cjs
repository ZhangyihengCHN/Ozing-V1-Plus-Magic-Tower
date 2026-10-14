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
  // Real merchant offers use a separate modal callback. Cancel must reach
  // its own hit area rather than being translated to the accept key.
  const merchantFixture = floor => {
    const b=Buffer.from(base),map=254+(floor-1)*121;
    const index=b.subarray(map,map+121).indexOf(0x32);
    assert.ok(index>=0,'merchant exists on floor '+floor);
    const dy=index<110?1:-1;
    b[5]=floor;b.writeUInt32LE(50,9);b.writeUInt32LE(10000,0x52);
    b.writeUInt32LE(10,0x5a);b.writeUInt32LE(index%11,0x76);
    b.writeUInt32LE(Math.floor(index/11)+dy,0x7a);b[map+index+11*dy]=0;
    return b;
  };
  const merchantOpen = (floor=28) => {key([15,31].includes(floor)?40:38);assert.equal(e.emu_dialog_active(),1);};
  for(const floor of [2,6,7,12,15,28,31,38,39,45,47]) {
    load(merchantFixture(floor));merchantOpen(floor);
    const dialogPixels=new Uint32Array(e.memory.buffer,e.emu_framebuffer(),240*320);
    for(const [x,y] of [[33,41],[207,41],[33,65],[207,65]])
      assert.equal(dialogPixels[y*240+x],0xffffff,'NPC name caption is opaque on floor '+floor);
    let nameInk=0;
    for(let y=44;y<64;y++)for(let x=40;x<200;x++)if(dialogPixels[y*240+x]===0)nameInk++;
    assert.ok(nameInk>20,'original NPC name glyphs remain visible');
    const dialogFrame=Buffer.from(dialogPixels);
    for(let n=0;n<5;n++){e.emu_paint();check();assert.deepEqual(Buffer.from(dialogPixels),dialogFrame,'NPC caption survives repaints');}
    key(27);const original=save();
    merchantOpen(floor);touch(150,188);assert.equal(e.emu_dialog_active(),0,'tap later closes NPC offer on floor '+floor);
    assert.deepEqual(save(),original,'cancelling preserves gold, keys, items, stats and merchant on floor '+floor);
    merchantOpen(floor);touch(40,110);assert.equal(e.emu_dialog_active(),1,'tap on NPC text does not accept');
    touch(150,188);assert.deepEqual(save(),original);
    merchantOpen(floor);key(27);assert.equal(e.emu_dialog_active(),0);assert.deepEqual(save(),original);
  }
  load(merchantFixture(28));merchantOpen();key(27);const beforeSale=save();
  merchantOpen();touch(88,188);assert.equal(e.emu_dialog_active(),0);
  const sold=save();assert.equal(sold.readUInt32LE(0x5a),9);assert.equal(sold.readUInt32LE(0x52),10100);
  load(merchantFixture(28));merchantOpen();key(41);assert.deepEqual(save(),sold,'keyboard accept retains original sale rules');
  load(merchantFixture(6));merchantOpen(6);touch(88,188);
  for(let i=0;i<4&&e.emu_dialog_active();i++)key(41);
  assert.equal(e.emu_dialog_active(),0,'dismiss the original post-purchase information');
  const bought=save();assert.equal(bought.readUInt32LE(0x52),9950);assert.equal(bought.readUInt32LE(0x5e),1);
  load(merchantFixture(2));merchantOpen(2);touch(88,188);assert.equal(e.emu_dialog_active(),0);
  const blessed=save();assert.equal(blessed.readUInt32LE(0x4a),103);assert.equal(blessed.readUInt32LE(0x4e),103);
  assert.equal(blessed.readUInt32LE(0x52),10000,'accepted blessing retains its original free 3% increase');
  for(let i=0;i<30;i++){load(beforeSale);merchantOpen();touch(150,188);assert.deepEqual(save(),beforeSale);}
  const checks=['four original shop floors','tap health / attack / defense / leave',
    'direction selection and confirmation','original price increases and gold deductions',
    'upgrades survive save / load','insufficient gold dialogue','movement after closing','100 reopen / cancel / save cycles',
    'original firmware background colour','stable popup redraw without Web selection overlay',
    'opaque original NPC name captions and stable redraw on all eleven offer floors',
    'NPC cancel and text taps preserve the full save on all eleven offer floors, including the rescued elder blessing',
    'original key sale / purchase and keyboard accept / return', '30 NPC cancel and reopen cycles'];
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

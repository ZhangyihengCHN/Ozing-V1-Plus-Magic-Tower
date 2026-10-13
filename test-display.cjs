// Verify native popup drawing and real firmware combat frames, using private saves.
const fs=require('node:fs');
const path=require('node:path');
const crypto=require('node:crypto');
const assert=require('node:assert/strict');
async function main(){
  const {instance}=await WebAssembly.instantiate(fs.readFileSync(path.join(__dirname,'game.wasm')));
  const e=instance.exports,root=path.join(__dirname,'verification');
  const check=()=>assert.equal(e.emu_last_error(),0,`ARM error at ${e.emu_last_error_pc().toString(16)}`);
  const key=k=>{assert.equal(e.emu_key(k),1);e.emu_paint();check();};
  const touch=(x,y)=>{assert.equal(e.web_touch(x,y),1);e.emu_paint();check();};
  const pixels=()=>new Uint32Array(e.memory.buffer,e.emu_framebuffer(),240*320);
  const rgba=()=>{const p=pixels(),b=Buffer.alloc(p.length*4);for(let i=0;i<p.length;i++){
    b[i*4]=p[i]>>>16;b[i*4+1]=p[i]>>>8;b[i*4+2]=p[i];b[i*4+3]=255;
  }return b;};
  const hash=()=>crypto.createHash('sha256').update(rgba()).digest('hex');
  const save=()=>{assert.equal(e.web_checkpoint(),1);assert.equal(e.emu_save_export(e.web_scratch(),8192),1);
    return Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304));};
  e.emu_init();key(41);key(41);const base=save();
  const load=b=>{assert.equal(e.emu_init(),1);new Uint8Array(e.memory.buffer,e.web_scratch(),b.length).set(b);
    assert.equal(e.emu_save_import(e.web_scratch(),b.length),1);key(40);key(41);};
  // Compare real map stairs with the original bitmap, rather than substitute
  // artwork. Downstairs (tile 7) intentionally has the vertical railing lines.
  const atlas=fs.readFileSync(path.join(__dirname,'assets/bmp00.bin'));
  const atlasOffset=atlas.readUInt32LE(10),atlasStride=(atlas.readInt32LE(18)+3)&~3;
  for(const [floor,tile,x,y] of [[1,6,0,0],[2,7,0,0],[3,7,0,10]]){
    const b=Buffer.from(base);b[5]=floor;load(b);
    for(let yy=0;yy<20;yy++)for(let xx=0;xx<20;xx++){
      const index=atlas[atlasOffset+(19-yy)*atlasStride+tile*20+xx],pal=54+4*index;
      const original=(atlas[pal+2]<<16)|(atlas[pal+1]<<8)|atlas[pal];
      assert.equal(pixels()[(24+y*20+yy)*240+10+x*20+xx],original,`floor ${floor} original stair pixel ${xx},${yy}`);
    }
  }
  load(base);
  assert.equal(e.web_book(),1);assert.equal(e.emu_dialog_active(),0,'book remains locked until obtained');check();
  for(const expanded of [0,1]){
    const b=Buffer.from(base);b.writeUInt32LE(b.readUInt32LE(0x3d)|1,0x3d);b.writeUInt32LE(expanded,0x21);
    load(b);const beforeBook=save();
    for(let i=0;i<30;i++){
      assert.equal(e.web_book(),1);assert.equal(e.emu_dialog_active(),1,'one shortcut opens book from either inventory view');check();
      assert.equal(e.web_book_active(),1);assert.equal(e.web_can_book(),1,'open book remains closable');
      assert.equal(e.web_book(),1);assert.equal(e.emu_dialog_active(),0);assert.equal(e.web_book_active(),0);assert.deepEqual(save(),beforeBook,'book toggle does not alter inventory view or save');
    }
  }
  const equipmentWithBook=Buffer.from(base);equipmentWithBook.writeUInt32LE(equipmentWithBook.readUInt32LE(0x3d)|1,0x3d);load(equipmentWithBook);
  const equipmentStats=[e.emu_hp(),e.emu_atk(),e.emu_def()];
  touch(219,310);assert.equal(e.emu_dialog_active(),1);assert.equal(e.web_book_active(),0);assert.equal(e.web_can_book(),0);assert.equal(e.web_book(),0);assert.equal(e.emu_dialog_active(),1,'book button cannot close the equipment dialog');
  assert.equal(pixels()[200*240+200],0xffffff,'equipment has an opaque background');
  const popupHash=hash();for(let i=0;i<30;i++){e.emu_paint();assert.equal(hash(),popupHash);}
  const equipmentFrame=rgba();key(27);assert.equal(e.emu_dialog_active(),0);
  assert.deepEqual([e.emu_hp(),e.emu_atk(),e.emu_def()],equipmentStats);save();
  touch(219,310);touch(120,120);assert.equal(e.emu_dialog_active(),0);save();
  const fixture=id=>{const b=Buffer.from(base);b.fill(0,254,375);b[254+4*11+5]=id;
    b.writeUInt32LE(5,0x76);b.writeUInt32LE(5,0x7a);return b;};
  const cases=[];let firstStrike,secondFrame;
  for(const [monster,expectedHp,expectedGold] of [[0xc0,1000,1],[0xc3,160,40]]){
    load(fixture(monster));const before=hash();key(38);
    assert.equal(e.web_combat_active(),1);assert.notEqual(hash(),before,'strike is visible before first tick');
    assert.equal(e.web_checkpoint(),0,'cannot save mid-combat');
    const firstHash=hash();if(monster===0xc3)firstStrike=rgba();
    key(39);touch(219,310);assert.equal(e.emu_x(),5);assert.equal(e.emu_y(),4);
    assert.equal(e.web_book(),0,'book cannot interrupt combat');
    assert.equal(e.emu_dialog_active(),0);assert.equal(hash(),firstHash,'input must not erase strike');
    for(let i=0;i<20;i++){e.emu_paint();assert.equal(hash(),firstHash,'refresh preserves strike');}
    const frames=[firstHash];let ticks=0;
    while(e.web_combat_active()&&ticks<100){
      assert.equal(e.emu_timer(2),1);check();const nativeFrame=hash();e.emu_paint();check();
      if(e.web_combat_active())assert.equal(hash(),nativeFrame,'native animation must survive host repaint');
      if(monster===0xc3&&ticks===1)secondFrame=rgba();
      frames.push(hash());ticks++;
    }
    assert.equal(e.web_combat_active(),0);assert.ok(ticks<100);
    const b=save();assert.equal(e.emu_hp(),expectedHp);assert.equal(b.readUInt32LE(0x52),expectedGold);
    assert.equal(b[254+4*11+5],0,'defeated monster is removed');
    assert.ok(new Set(frames).size>=2,'combat has visibly distinct frames');
    if(monster===0xc3)assert.ok(ticks>=8,'multi-hit combat advances through both attack phases');
    load(b);assert.equal(e.emu_hp(),expectedHp);assert.equal(save().readUInt32LE(0x52),expectedGold);
    key(40);assert.equal(e.emu_y(),5,'can move after combat');
    cases.push({monster:monster.toString(16),ticks,distinct_frames:new Set(frames).size,hp:expectedHp,gold:expectedGold});
  }
  for(const [name,frame] of [['equipment-fixed',equipmentFrame],['combat-strike',firstStrike],['combat-recovery',secondFrame]])
    fs.writeFileSync(path.join(root,name+'.rgba'),frame);
  const wrap=b=>JSON.stringify({format:'hjx-magic-tower-portrait-v1',version:1,data:b.toString('base64')});
  fs.writeFileSync(path.join(root,'combat-fixture.json'),wrap(fixture(0xc3)));
  const report={result:'PASS',checks:['original upstairs and downstairs artwork on floors 1 / 2 / 3',
    'same button opens/closes the original book from both inventory views','book ownership check and 60 open / close / save cycles',
    'opaque and stable equipment popup','equipment close by return and touch; book toggle cannot close equipment',
    'original strike sprite for one-hit combat','timer-driven multi-hit animation',
    'repaint and input preserve animation','no saving during combat','original damage and rewards',
    'save / load after combat','movement after battle'],cases};
  fs.writeFileSync(path.join(root,'display.json'),JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify(report,null,2));
}
main().catch(error=>{console.error(error);process.exitCode=1;});

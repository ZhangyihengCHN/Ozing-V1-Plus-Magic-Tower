const projectRoot = require('./setup.cjs');
const fs=require('node:fs'),assert=require('node:assert/strict');
(async()=>{
 const e=(await WebAssembly.instantiate(fs.readFileSync('game.wasm'))).instance.exports;
 e.emu_init();e.emu_key(41);e.emu_key(41);e.web_checkpoint();e.emu_save_export(e.web_scratch(),8192);
 const base=Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304));
 const load=b=>{e.emu_init();new Uint8Array(e.memory.buffer,e.web_scratch(),6304).set(b);assert.equal(e.emu_save_import(e.web_scratch(),6304),1);};
 const exported=()=>{assert.equal(e.emu_save_export(e.web_scratch(),8192),1);return Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304));};
 const fixture=Buffer.from(base);fixture.writeUInt32LE(0x100,0x3d);fixture.fill(0,254,375);for(const off of [0x76,0x7a,0x7e,0x82])fixture.writeUInt32LE(5,off);
 for(const count of [0,1,2,3,4,999,0xffffffff]){
  const source=Buffer.from(fixture);source.writeUInt32LE(count,0x41);load(source);
  const expected=Buffer.from(source);expected.writeUInt32LE(Math.min(count,3),0x41);assert.deepEqual(exported(),expected,'only the legacy charge field changes');
  e.emu_key(40);e.emu_key(41);assert.equal(e.web_item_count(6),Math.min(count,3));assert.equal(e.emu_last_error(),0);
 }
 const legacy=Buffer.from(fixture);legacy.writeUInt32LE(999,0x41);load(legacy);e.emu_key(40);e.emu_key(41);
 for(const remaining of [2,1,0]){assert.equal(e.web_item_use(6),1);assert.equal(e.web_item_count(6),remaining);}
 assert.equal(e.web_item_owned(6),0);assert.equal(e.web_item_use(6),0);assert.equal(e.web_checkpoint(),1);
 const depleted=exported();load(depleted);e.emu_key(40);e.emu_key(41);assert.equal(e.web_item_count(6),0);assert.equal(e.web_item_owned(6),0);
 // The firmware's real pickup also grants exactly three, without a Web grant.
 const pickup=Buffer.from(fixture);pickup.writeUInt32LE(0,0x41);pickup.writeUInt32LE(0,0x3d);pickup[254+49]=0x3c;
 load(pickup);e.emu_key(40);e.emu_key(41);e.emu_key(38);assert.equal(e.web_item_count(6),3);assert.equal(e.emu_last_error(),0);
 const proof=JSON.parse(fs.readFileSync('verification/quick-items-fixture.json'));const old=Buffer.from(proof.data,'base64');old.writeUInt32LE(999,0x41);
 fs.writeFileSync('verification/symmetric-flight-legacy-fixture.json',JSON.stringify({...proof,data:old.toString('base64')}));
 const report={result:'PASS',checks:['original pickup grants3 charges','legacy counts4/999/uint32max restore to3 on native import','other6300 snapshot bytes remain exact','counts0/1/2/3 remain unchanged','successful uses count down3/2/1/0 and remove the depleted item','loading depleted progress does not refill it']};
 fs.writeFileSync('verification/item-limits.json',JSON.stringify(report,null,2));console.log(JSON.stringify(report,null,2));
})().catch(err=>{console.error(err);process.exitCode=1;});

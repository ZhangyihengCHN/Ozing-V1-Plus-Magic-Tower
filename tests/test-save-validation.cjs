const projectRoot = require('./setup.cjs');
const fs=require('node:fs'),assert=require('node:assert/strict');
(async()=>{
 const e=(await WebAssembly.instantiate(fs.readFileSync('game.wasm'))).instance.exports;
 e.emu_init();e.emu_key(41);e.emu_key(41);assert.equal(e.web_checkpoint(),1);assert.equal(e.emu_save_export(e.web_scratch(),8192),1);
 const original=Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304));
 const cases=[[5,0xffffffff],[5,257],[5,51],[9,0],[9,51],[0x76,11],[0x7a,0xffffffff],[0x7e,11],[0x82,11],[0x1d,0],[0x21,2],[0x46,0],[0x46,0xffffffff],[0x4a,0x80000000],[0x4e,0xffffffff],[0x52,0xffffffff],[0x5a,0xffffffff],[0x5e,0xffffffff],[0x62,0xffffffff]];
 for(const [off,val]of cases){const b=Buffer.from(original);b.writeUInt32LE(val,off);new Uint8Array(e.memory.buffer,e.web_scratch(),6304).set(b);assert.equal(e.emu_save_import(e.web_scratch(),6304),0,`refuse offset ${off.toString(16)} = ${val}`);assert.equal(e.emu_save_export(e.web_scratch(),8192),1);assert.deepEqual(Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304)),original,'rejected imports cannot replace the previous native snapshot');}
 const basement=Buffer.from(original);basement.writeUInt32LE(0,5);new Uint8Array(e.memory.buffer,e.web_scratch(),6304).set(basement);assert.equal(e.emu_save_import(e.web_scratch(),6304),1);assert.equal(e.emu_save_export(e.web_scratch(),8192),1);assert.deepEqual(Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304)),basement);
 const report={result:'PASS',checks:['19 malformed floor/coordinate/state/stat cases rejected in native import','full-width floor checked instead of only its low byte','last valid native snapshot preserved after every rejection','legal basement floor zero accepted and preserved']};fs.writeFileSync('verification/save-validation.json',JSON.stringify(report,null,2));console.log(JSON.stringify(report,null,2));
})().catch(e=>{console.error(e);process.exitCode=1;});

const projectRoot = require('./setup.cjs');
// Check actual firmware pickup messages; use only private test saves.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
async function main(){
 const e=(await WebAssembly.instantiate(fs.readFileSync(path.join(projectRoot,'game.wasm')))).instance.exports;
 const check=()=>assert.equal(e.emu_last_error(),0,e.emu_last_error_pc().toString(16));
 const key=k=>{e.emu_key(k);e.emu_paint();check();};
 const save=()=>{assert.equal(e.web_checkpoint(),1);e.emu_save_export(e.web_scratch(),8192);return Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304));};
 e.emu_init();key(41);key(41);const base=save();
 const load=b=>{e.emu_init();new Uint8Array(e.memory.buffer,e.web_scratch(),6304).set(b);e.emu_save_import(e.web_scratch(),6304);key(40);key(41);};
 const bar=()=>{const p=new Uint32Array(e.memory.buffer,e.emu_framebuffer(),240*320),b=Buffer.alloc(190*20*4);for(let y=0;y<20;y++)for(let x=0;x<190;x++)b.writeUInt32LE(p[(300+y)*240+x],(y*190+x)*4);return b;};
 const checks=[];let proof;
 for(const [name,id,offset,expected] of [['yellow key',0x1b,0x5a,1],['iron sword',0x22,0x4a,110],['iron shield',0x2a,0x4e,110]]){
  const b=Buffer.from(base);b.fill(0,254,375);b[254+4*11+5]=id;b.writeUInt32LE(5,0x76);b.writeUInt32LE(5,0x7a);load(b);
  const empty=bar();key(38);const picked=bar();assert.notDeepEqual(picked,empty,name+' displays native pickup text');
  const acquired=save();assert.equal(acquired.readUInt32LE(offset),expected,name+' original effect');assert.equal(acquired[254+4*11+5],0);
  for(let i=0;i<30;i++){e.emu_paint();check();assert.deepEqual(bar(),picked,'repainting preserves pickup message');}
  assert.deepEqual(save(),acquired,'repainting never duplicates pickup or changes the save');
  const fixture={format:'hjx-magic-tower-portrait-v1',version:1,data:b.toString('base64')};
  if(name==='iron sword'){
   fs.writeFileSync(path.join(projectRoot,'verification','pickup-fixture.json'),JSON.stringify(fixture));
   const p=new Uint32Array(e.memory.buffer,e.emu_framebuffer(),240*320);proof=Buffer.alloc(p.length*4);
   for(let i=0;i<p.length;i++){proof[i*4]=p[i]>>>16;proof[i*4+1]=p[i]>>>8;proof[i*4+2]=p[i];proof[i*4+3]=255;}
  }
  key(39);assert.deepEqual(bar(),empty,'next movement clears the previous message');
  load(acquired);assert.deepEqual(bar(),empty,'loading clears transient messages');
  checks.push(name+' pickup, repaint, original stats, save and message clearing');
 }
 fs.writeFileSync(path.join(projectRoot,'verification','pickup-native.rgba'),proof);
 const report={result:'PASS',checks};fs.writeFileSync(path.join(projectRoot,'verification','notice.json'),JSON.stringify(report,null,2));console.log(JSON.stringify(report,null,2));
}
main().catch(err=>{console.error(err);process.exitCode=1;});

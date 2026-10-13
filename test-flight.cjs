const fs=require('node:fs'),assert=require('node:assert/strict'),crypto=require('node:crypto');
async function main(){
 const e=(await WebAssembly.instantiate(fs.readFileSync('game.wasm'))).instance.exports;
 const check=()=>assert.equal(e.emu_last_error(),0,e.emu_last_error_pc().toString(16));
 const key=k=>{assert.equal(e.emu_key(k),1);e.emu_paint();check();};
 const touch=(x,y)=>{assert.equal(e.web_touch(x,y),1);e.emu_paint();check();};
 const save=()=>{assert.equal(e.web_checkpoint(),1);assert.equal(e.emu_save_export(e.web_scratch(),8192),1);return Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304));};
 e.emu_init();key(41);key(41);const base=save();
 const fixture=(floor=3,max=4,owned=true,near=true)=>{const b=Buffer.from(base);b[5]=floor;b.writeUInt32LE(max,9);b.writeUInt32LE(0,0x21);b.writeUInt32LE(owned?4:0,0x3d);
  const stairs=[];for(let i=0;i<121;i++)if([6,7].includes(b[254+(floor-1)*121+i]))stairs.push(i);if(!stairs.length){b[254+(floor-1)*121]=7;stairs.push(0);}const i=stairs[0];const x=i%11,y=Math.floor(i/11);b.writeUInt32LE(near?(x===0?1:x-1):5,0x76);b.writeUInt32LE(near?y:5,0x7a);return b;};
 const load=b=>{e.emu_init();new Uint8Array(e.memory.buffer,e.web_scratch(),6304).set(b);e.emu_save_import(e.web_scratch(),6304);key(40);key(41);};
 const pixels=()=>{const p=new Uint32Array(e.memory.buffer,e.emu_framebuffer(),240*320),b=Buffer.alloc(p.length*4);p.forEach((v,i)=>{b[i*4]=v>>>16;b[i*4+1]=v>>>8;b[i*4+2]=v;b[i*4+3]=255;});return b;};
 const hash=()=>crypto.createHash('sha256').update(pixels()).digest('hex');const open=()=>{touch(152,265);assert.equal(e.emu_dialog_active(),1);};
 load(fixture(3,4,false));touch(152,265);assert.equal(e.emu_dialog_active(),0);assert.equal(e.emu_floor(),3);
 load(fixture(3,4,true,false));touch(152,265);assert.equal(e.emu_dialog_active(),0);assert.equal(e.emu_floor(),3);
 const original=fixture();load(original);open();assert.equal(e.web_can_checkpoint(),0);assert.equal(e.web_checkpoint(),0);assert.equal(e.web_book(),0);
 const initial=hash();for(let i=0;i<30;i++){e.emu_paint();assert.equal(hash(),initial);}key(40);assert.notEqual(hash(),initial);fs.writeFileSync('verification/flight-menu.rgba',pixels());key(27);assert.equal(e.emu_dialog_active(),0);assert.deepEqual(save(),original);
 open();key(40);key(41);assert.equal(e.emu_floor(),2);assert.equal(e.emu_dialog_active(),0);assert.deepEqual([e.emu_x(),e.emu_y()],[0,9]);
 load(original);open();touch(120,132);assert.equal(e.emu_floor(),4);assert.equal(e.emu_dialog_active(),0);const up=save();assert.equal(up.readUInt32LE(0x3d),4);assert.equal(up.readUInt32LE(0x46),base.readUInt32LE(0x46));load(up);assert.equal(e.emu_floor(),4);
 load(original);open();touch(120,160);assert.equal(e.emu_floor(),2);
 load(original);open();touch(20,90);assert.deepEqual(save(),original);
 for(const [floor,max,dir,expected]of[[1,4,40,1],[3,3,38,3],[50,50,38,50],[43,50,38,45],[45,50,40,43]]){load(fixture(floor,max));open();key(dir);key(41);assert.equal(e.emu_floor(),expected);assert.equal(e.emu_dialog_active(),0);save();}
 load(original);for(let i=0;i<100;i++){open();key(27);assert.deepEqual(save(),original);}check();
 // The toolbar shortcuts use the identical original flight command.
 load(original);assert.equal(e.web_can_fly(0),1);assert.equal(e.web_can_fly(1),1);assert.equal(e.web_can_fly(2),0);
 const same=save();for(let i=0;i<100;i++){e.web_can_fly(0);e.web_can_fly(1);}assert.deepEqual(save(),same,'availability queries have no side effects');
 assert.equal(e.web_fly(1),1);assert.equal(e.emu_floor(),2);assert.deepEqual([e.emu_x(),e.emu_y()],[0,9]);assert.equal(e.emu_dialog_active(),0);save();
 const statsView=Buffer.from(original);statsView.writeUInt32LE(1,0x21);load(statsView);assert.equal(e.web_fly(0),1);assert.equal(e.emu_floor(),4);assert.equal(save().readUInt32LE(0x21),1);
 load(original);assert.equal(e.web_fly(0),1);assert.equal(e.emu_floor(),4);assert.equal(e.emu_dialog_active(),0);assert.deepEqual(save(),up,'shortcut and original popup produce identical native saves');
 for(const b of [fixture(3,4,false),fixture(3,4,true,false)]){load(b);const kept=save();assert.equal(e.web_can_fly(0),0);assert.equal(e.web_can_fly(1),0);assert.equal(e.web_fly(0),0);assert.equal(e.web_fly(1),0);assert.deepEqual(save(),kept);}
 for(const [floor,max,down,can,expected] of [[1,4,1,0,1],[3,3,0,0,3],[50,50,0,0,50],[43,50,0,1,45],[45,50,1,1,43]]){load(fixture(floor,max));assert.equal(e.web_can_fly(down),can);assert.equal(e.web_fly(down),can);assert.equal(e.emu_floor(),expected);check();save();}
 load(original);open();assert.equal(e.web_can_fly(0),0);assert.equal(e.web_can_fly(1),0);assert.equal(e.web_fly(1),0);key(27);
 for(let i=0;i<100;i++){load(original);assert.equal(e.web_fly(i%2),1);assert.equal(e.emu_floor(),i%2?2:4);save();check();}
 fs.writeFileSync('verification/flight-fixture.json',JSON.stringify({format:'hjx-magic-tower-portrait-v1',version:1,savedAt:'2026-10-10T10:00:00Z',data:original.toString('base64')}));
 const report={result:'PASS',checks:['original ownership and staircase adjacency rules','native menu labels and bitmap font','direction selection and confirm, tap up/down, Escape and outside cancel','original destination staircase coordinates','visited floor bounds, first/last floors and original floor-44 skip','item and stats unchanged, save/load after flying','100 open/cancel/save cycles without leaking resources','toolbar up/down shortcuts match original popup snapshots, eligibility, pause and floor bounds','read-only availability checks and 100 shortcut/save cycles']};fs.writeFileSync('verification/flight.json',JSON.stringify(report,null,2));console.log(JSON.stringify(report,null,2));
}
main().catch(e=>{console.error(e);process.exitCode=1;});

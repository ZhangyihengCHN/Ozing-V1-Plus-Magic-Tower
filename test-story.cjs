// Real ARM story events must not wait for the next physical direction key.
const fs=require('node:fs'),assert=require('node:assert/strict'),crypto=require('node:crypto');
async function main(){
 const e=(await WebAssembly.instantiate(fs.readFileSync('game.wasm'))).instance.exports;
 const check=()=>assert.equal(e.emu_last_error(),0,`ARM error ${e.emu_last_error_pc().toString(16)}`);
 const key=k=>{assert.equal(e.emu_key(k),1);e.emu_paint();check();};
 const save=()=>{assert.equal(e.web_checkpoint(),1);assert.equal(e.emu_save_export(e.web_scratch(),8192),1);return Buffer.from(new Uint8Array(e.memory.buffer,e.web_scratch(),6304));};
 const load=b=>{assert.equal(e.emu_init(),1);new Uint8Array(e.memory.buffer,e.web_scratch(),6304).set(b);assert.equal(e.emu_save_import(e.web_scratch(),6304),1);key(40);key(41);};
 const state=()=>[e.emu_floor(),e.emu_x(),e.emu_y(),e.emu_hp(),e.emu_atk(),e.emu_def()];
 const pixels=()=>{const p=new Uint32Array(e.memory.buffer,e.emu_framebuffer(),240*320),b=Buffer.alloc(p.length*4);p.forEach((v,i)=>{b[i*4]=v>>>16;b[i*4+1]=v>>>8;b[i*4+2]=v;b[i*4+3]=255;});return b;};
 e.emu_init();key(41);key(41);const base=save();
 const fixture=(floor,thief=2)=>{const b=Buffer.from(base);b[5]=floor;b.writeUInt32LE(40,9);b.writeUInt32LE(4,0x3d);b.writeUInt32LE(0,0x21);b.writeUInt32LE(thief,0x86);const map=254+(floor-1)*121;const cells=b.subarray(map,map+121);const i=cells.indexOf(6)>=0?cells.indexOf(6):cells.indexOf(7);assert.ok(i>=0);b.writeUInt32LE(i%11===0?1:i%11-1,0x76);b.writeUInt32LE(Math.floor(i/11),0x7a);return b;};
 const thiefIndex=254+34*121+103;
 // Fly from the real 34th-floor stair to the original 35th-floor landing.
 const arrival=fixture(34);load(arrival);e.web_touch(152,265);key(41);assert.equal(e.emu_floor(),35);assert.equal(save()[thiefIndex],0);
 const standing=state(),before=pixels();assert.equal(e.web_tick(16),2);check();assert.deepEqual(state(),standing,'idle event must not move the warrior or change combat stats');
 assert.equal(save()[thiefIndex],0x30,'rescued thief appears without any direction input');assert.notDeepEqual(pixels(),before,'event redraw reaches the framebuffer');
 const appeared=save();for(let i=0;i<100;i++)assert.equal(e.web_tick(16),1);assert.deepEqual(save(),appeared,'idle frames cannot repeat the event or change saves');
 // Read the same floor directly; only the original rescue flag permits arrival.
 const on35=fixture(35);load(on35);assert.equal(e.web_tick(16),2);assert.equal(save()[thiefIndex],0x30);
 load(fixture(35,0));assert.equal(e.web_tick(16),2);assert.equal(save()[thiefIndex],0,'unrescued thief must not be injected');
 // Touch/keyboard staircase entry already runs the original event pass.
 load(arrival);key(39);assert.equal(e.emu_floor(),35);assert.equal(save()[thiefIndex],0x30);assert.equal(e.web_tick(16),1);
 // A saved original story trigger opens its dialogue while standing still.
 const dialogue=fixture(40,0);dialogue.writeUInt32LE(5,0x76);dialogue.writeUInt32LE(6,0x7a);dialogue.writeUInt32LE(1,0xd2);
 assert.equal(dialogue[254+39*121+71],0,'real trigger tile is empty');load(dialogue);const pos=state();assert.equal(e.emu_dialog_active(),0);assert.equal(e.web_tick(16),2);assert.equal(e.emu_dialog_active(),1);assert.deepEqual(state(),pos);
 fs.writeFileSync('verification/story-dialog.rgba',pixels());for(let i=0;i<20;i++){assert.equal(e.web_tick(16),1);assert.equal(e.emu_dialog_active(),1);assert.deepEqual(state(),pos);}key(41);assert.equal(e.emu_dialog_active(),0);assert.equal(e.web_tick(16),1);save();
 // The firmware synchronously sends a zero-key event after a battle. This
 // used to be dropped by the RTOS host, leaving the completion dialogue pending.
 const battle=fixture(40,0),map=254+39*121,guards=[12,13,14,18,19,20,35,36,37,39,40,41];assert.ok(battle[map+12]>=0xc0);for(const i of guards.slice(1))battle[map+i]=0;
 battle.writeUInt32LE(0,0xd2);battle.writeUInt32LE(1,0xd6);battle.writeUInt32LE(0,0xda);battle.writeUInt32LE(1,0x76);battle.writeUInt32LE(2,0x7a);battle[map+23]=0;
 battle.writeUInt32LE(1000000,0x46);battle.writeUInt32LE(1000000,0x4a);battle.writeUInt32LE(1000000,0x4e);load(battle);e.web_tick(16);key(38);assert.equal(e.web_combat_active(),1);assert.equal(e.emu_dialog_active(),0);
 for(let i=0;i<30&&!e.emu_dialog_active();i++){assert.equal(e.emu_timer(2),1);e.emu_paint();check();}
 assert.equal(e.emu_dialog_active(),1,'last guard completion dialogue opens from the battle timer without a key');fs.writeFileSync('verification/story-after-battle.rgba',pixels());key(41);assert.equal(e.emu_dialog_active(),0);e.web_tick(16);save();
 // Capture the original menu pause: no background event may run into it.
 load(arrival);e.web_touch(152,265);assert.equal(e.emu_dialog_active(),1);assert.equal(e.web_tick(16),1);assert.equal(e.emu_floor(),34);key(27);
 for(let i=0;i<100;i++){load(on35);assert.equal(e.web_tick(16),2);assert.equal(save()[thiefIndex],0x30);check();}
 const json=b=>JSON.stringify({format:'hjx-magic-tower-portrait-v1',version:1,savedAt:'2026-10-10T12:00:00Z',data:b.toString('base64')});
 fs.writeFileSync('verification/story-arrival-fixture.json',json(arrival));fs.writeFileSync('verification/story-dialog-fixture.json',json(dialogue));
 const report={result:'PASS',checks:['flight arrival on floor 35 spawns the rescued thief without movement','loading a floor runs original events once; unrescued thief remains absent','physical stairs do not repeat their original event pass','dialogue opens and stays visible while standing still','battle timer delivers the original completion event without another key','active menu/dialogue pauses event delivery','100 restore/event/save cycles with stable idle frames and no ARM errors']};fs.writeFileSync('verification/story.json',JSON.stringify(report,null,2));console.log(JSON.stringify(report,null,2));
}
main().catch(err=>{console.error(err);process.exitCode=1;});

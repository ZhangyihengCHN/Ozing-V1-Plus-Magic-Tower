/* Plain browser JavaScript. The original ARM game owns maps, events and combat. */
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const canvas = $('screen'), ctx = canvas.getContext('2d', {alpha:false});
  const titlePixels = ctx.createImageData(240, 320);
  const gamePixels = ctx.createImageData(240, 296);
  let viewTop = 0;
  const STORE = 'hjx-magic-tower-portrait-v1';
  const SLOT_STORE = 'hjx-magic-tower-portrait-slots-v1';
  const SLOT_COUNT = 3;
  const SECURITY_STORE = 'hjx-magic-tower-save-security-v2';
  let pendingMigration = null;
  let slotMode = null, pendingSave = null, overwriteSlot = null, pickerReturn = null, slotCursor = 0, overwriteConfirm = false;
  const entryScript = document.currentScript;
  const runtimePath = entryScript?.dataset.runtime || 'game-data.js';
  const runtimeSize = Number(entryScript?.dataset.runtimeSize) || 0;
  let core, gameReady = false, paused = false, last = 0, timers = new Map(), dirty = false;
  let lastSave = 0, held = null, repeatAt = 0, lastGeneration = 0;
  let hasStoredSave = false;
  const joystick = $('joystick');
  const directionButtons = Array.from(joystick.querySelectorAll('[data-key]'));
  const bookButton = document.querySelector('[data-key="66"]');
  const itemNames=['飞行仗','冰冻魔法','镐','地震卷轴','炸弹','圣水','飞行道具','上楼道具','下楼道具','魔法钥匙'];
  const itemButtons=Array.from({length:10},(_,slot)=>$('quick-'+slot));
  const itemToolState=[];
  let joystickPointer = null;
  const keys = {ArrowUp:38,ArrowDown:40,ArrowLeft:37,ArrowRight:39,
    w:38,W:38,s:40,S:40,a:37,A:37,d:39,D:39,Enter:41,' ':41,z:41,Z:41,Escape:27,x:27,X:27,b:66,B:66,n:78,N:78};
  const physicalKeys = {KeyW:38,KeyS:40,KeyA:37,KeyD:39,KeyZ:41,KeyX:27,KeyB:66,KeyN:78};
  const keyboardKey = event => physicalKeys[event.code] ?? keys[event.key];
  const focusGame = () => canvas.focus({preventScroll:true});
  const state = () => ({floor:core.emu_floor(),x:core.emu_x(),y:core.emu_y(),hp:core.emu_hp(),atk:core.emu_atk(),def:core.emu_def()});
  function check() {
    if (core.emu_last_error()) throw new Error(`游戏运行错误 ${core.emu_last_error()} / ${core.emu_last_error_pc().toString(16)}`);
  }
  function draw() {
    // Gameplay reserves 24 empty rows for the original system title bar.
    // Show only its game area; the original title screen keeps its full image.
    viewTop = core.emu_mode() === 0 ? 24 : 0;
    const height = 320 - viewTop;
    if(canvas.height !== height) canvas.height = height;
    const pixels = viewTop ? gamePixels : titlePixels;
    const source = new Uint32Array(core.memory.buffer, core.emu_framebuffer() + viewTop * 240 * 4, 240 * height);
    const target = pixels.data;
    for (let i=0,j=0;i<source.length;i++,j+=4) {
      target[j] = (source[i] >>> 16) & 255; target[j+1] = (source[i] >>> 8) & 255; target[j+2] = source[i] & 255; target[j+3] = 255;
    }
    ctx.putImageData(pixels,0,0);
  }
  function updateTools() {
    const ready = !!core && gameReady && !slotMode, canSave = ready && !!core.web_can_checkpoint();
    $('save').disabled = !canSave;
    $('menu-save').disabled = !canSave;
    $('continue').disabled = !ready || !hasStoredSave;
    bookButton.disabled = !ready || paused || !core.web_can_book();
    bookButton.setAttribute('aria-pressed',!!core && !!core.web_book_active()?'true':'false');
    $('fly-up').disabled = !ready || paused || !core.web_can_fly(0);
    $('fly-down').disabled = !ready || paused || !core.web_can_fly(1);
    itemButtons.forEach((button,slot)=>{
      const owned=!!core&&!!core.web_item_owned(slot),count=owned?core.web_item_count(slot):0;
      button.disabled=!ready||paused||!core.web_can_item(slot);
      const signature=owned+':'+count;if(itemToolState[slot]===signature)return;itemToolState[slot]=signature;
      if(owned)button.classList.add('owned');else button.classList.remove('owned');
      $('quick-icon-'+slot).hidden=!owned;
      $('quick-count-'+slot).textContent=owned&&slot===6?String(count):'';
      button.title=itemNames[slot]+'（'+((slot+1)%10)+'）'+(owned?'':' · 未获得');
      button.setAttribute('aria-label',button.title+(slot===6&&owned?' · 剩余 '+count+' 次':''));
    });
    $('export').disabled = !ready || !(canSave || hasStoredSave);
    $('import').disabled = !ready;
    directionButtons.forEach(button=>button.disabled=!(slotMode || ready && !paused));
    document.querySelectorAll('.controls > [data-key]').forEach(button=>button.disabled=!(slotMode || ready));
  }
  function refresh() {
    core.emu_paint();check();draw();updateTools();
  }
  function error(err) { setPaused(true);$('paused').querySelector('strong').textContent=err.message || '游戏运行出错';console.error(err); }
  function menuError(message){setMenu(true);$('menu-error').textContent=message;$('menu-error').hidden=false;}
  function act(key) {
    if(slotMode) { slotKey(key); return; }
    if (!core || !gameReady) return;
    if(paused){if(key===27 || key===41)setPaused(false);return;}
    if(core.web_busy() || core.web_combat_active()) return;
    if(key===27&&!core.emu_dialog_active()) { setMenu(true); return; }
    try {
      const before = JSON.stringify(state());
      // The shortcut toggles only the original book, regardless of inventory view.
      if(key===66 && core.emu_mode()===0) core.web_book();
      else core.emu_key(key);
      refresh();
      if (JSON.stringify(state()) !== before || core.emu_save_generation() !== lastGeneration ||
          core.emu_mode()===0 && [37,38,39,40].includes(key)) dirty = true;
    } catch (err) { error(err); }
  }
  function release() {
    held = null; joystickPointer = null;
    document.querySelectorAll('.pressed').forEach(button => button.classList.remove('pressed'));
  }
  function hold(key, button) {
    release(); act(key);
    if(!core || !slotMode && (paused || core.web_busy()) || button && button.disabled) return;
    if ([37,38,39,40].includes(key)) { held = key; repeatAt = performance.now() + 300; }
    if (button) button.classList.add('pressed');
  }
  function readStored() {
    try { const text = readLocal(STORE); return text ? validate(JSON.parse(text)) : null; }
    catch (_) { return null; }
  }
  function readSlots() {
    let text;
    try { text=readLocal(SLOT_STORE); } catch (_) {}
    if(!text) return {slots:[readStored(),null,null],selected:null,archivedSlots:[]};
    try {
      const data=JSON.parse(text);
      if(data.format!==SLOT_STORE || !Array.isArray(data.slots)) throw new Error('存档槽格式不正确');
      return {slots:Array.from({length:SLOT_COUNT},(_,i)=>{try{return data.slots[i]?validate(data.slots[i]):null;}catch(_){return null;}}),
        selected:Number.isInteger(data.selected)&&data.selected>=0&&data.selected<SLOT_COUNT?data.selected:null,
        archivedSlots:[...(Array.isArray(data.archivedSlots)?data.archivedSlots:[]),...data.slots.slice(SLOT_COUNT)]};
    } catch (_) { return {slots:[readStored(),null,null],selected:null,archivedSlots:[]}; }
  }
  function writeSlots(data) {
    try { finishMigration();localStorage.setItem(SLOT_STORE,JSON.stringify({format:SLOT_STORE,version:3,...data,slots:data.slots.map(save=>save?storedSave(save):null)})); }
    catch (_) { throw new Error('无法写入存档，请检查浏览器存储空间或导出进度'); }
  }
  function syncStored() { hasStoredSave=!!readStored() || readSlots().slots.some(Boolean); updateTools(); }
  function migrateSlots() {
    try { if(!localStorage.getItem(SLOT_STORE)) writeSlots(readSlots()); } catch (_) {}
  }
  function decode(text) { const binary = atob(text); return Uint8Array.from(binary, c => c.charCodeAt(0)); }
  function encode(bytes) { let text = ''; for (const byte of bytes) text += String.fromCharCode(byte); return btoa(text); }
  function validateLegacy(save) {
    if (!save || save.format !== STORE || save.version!==undefined&&save.version!==1 || typeof save.data !== 'string') throw new Error('不是竖屏魔塔存档');
    const bytes = decode(save.data);
    if (bytes.length !== 6304) throw new Error('存档长度不正确');
    // Original slot = 254-byte player state + 50 x 121-byte maps.
    const view = new DataView(bytes.buffer);
    const word=offset=>view.getUint32(offset,true),floor=word(5),visited=word(9);
    if (floor>50 || visited<1 || visited>50 || word(0x76)>10 || word(0x7a)>10 || word(0x7e)>10 || word(0x82)>10)
      throw new Error('存档中的楼层或位置不正确');
    if(word(0x1d)!==1 || word(0x21)>1 || !word(0x46) ||
       [0x46,0x4a,0x4e,0x52,0x5a,0x5e,0x62].some(offset=>word(offset)>0x7fffffff))
      throw new Error('存档中的状态或属性不正确');
    // The original pickup grants three symmetric flights. Repair legacy
    // development counts without refilling charges that were already used.
    const repairFlight=word(0x41)>3;
    if(repairFlight)view.setUint32(0x41,3,true);
    // Metadata always comes from the original binary, including imported saves.
    return {format:STORE,version:1,savedAt:save.savedAt,data:repairFlight?encode(bytes):save.data,state:{floor:bytes[5],x:view.getUint32(0x76,true),y:view.getUint32(0x7a,true),
      hp:view.getUint32(0x46,true),atk:view.getUint32(0x4a,true),def:view.getUint32(0x4e,true),gold:view.getUint32(0x52,true),
      yellow:view.getUint32(0x5a,true),blue:view.getUint32(0x5e,true),red:view.getUint32(0x62,true)}};
  }
  function storedSave(save) { return SaveCrypto.envelope(validate(save)); }
  function saveBytes(save) { return SaveCrypto.open(save).bytes; }
  function validate(save) {
    const opened=SaveCrypto.open(save),data=encode(opened.bytes);
    const plain=validateLegacy({format:STORE,version:1,savedAt:opened.savedAt,data});
    const envelope=plain.data===data?SaveCrypto.envelope(save):SaveCrypto.seal(decode(plain.data),plain.savedAt);
    return {...envelope,savedAt:plain.savedAt,state:plain.state};
  }
  function readLocal(key) {
    const text=localStorage.getItem(key),copy=pendingMigration?.get(key);
    return copy&&text===copy.before?copy.after:text;
  }
  function finishMigration() {
    if(!pendingMigration)return;
    try {
      for(const [key,copy] of pendingMigration)if(copy.after!==copy.before)localStorage.setItem(key,copy.after);
      localStorage.setItem(SECURITY_STORE,'1');pendingMigration=null;
    } catch(err) {
      for(const [key,copy] of pendingMigration)try{localStorage.setItem(key,copy.before);}catch(_){}
      throw err;
    }
  }
  function migrateEncryptedSaves() {
    try{if(localStorage.getItem(SECURITY_STORE)==='1')return;}catch(_){return;}
    const copies=new Map();
    const convert=save=>{
      if(!save)return save;
      if(save.version===2){try{return storedSave(save);}catch(_){return save;}}
      let plain;try{plain=validateLegacy(save);}catch(_){return save;}
      return SaveCrypto.seal(decode(plain.data),plain.savedAt);
    };
    for(const key of [STORE,SLOT_STORE]){
      const before=localStorage.getItem(key);if(!before)continue;
      let object;try{object=JSON.parse(before);}catch(_){continue;}
      if(key===STORE)object=convert(object);
      else if(object.format===SLOT_STORE&&Array.isArray(object.slots)){
        object={...object,version:3,slots:object.slots.map(convert),
          archivedSlots:Array.isArray(object.archivedSlots)?object.archivedSlots.map(convert):[]};
      }
      copies.set(key,{before,after:JSON.stringify(object)});
    }
    pendingMigration=copies;
    try{finishMigration();}catch(_){console.warn('本地存档暂未完成加密迁移，可读取原有进度，请导出备份或释放存储空间');}
  }
  function captureSave() {
    if (!core || !core.web_checkpoint()) return null;
    check();
    const pointer = core.web_scratch();
    if (!core.emu_save_export(pointer, 8192)) throw new Error('保存进度失败');
    return validate(SaveCrypto.seal(new Uint8Array(core.memory.buffer,pointer,6304).slice(),new Date().toISOString()));
  }
  function checkpoint(save=captureSave()) {
    if(!save)return null;
    try { finishMigration();localStorage.setItem(STORE,JSON.stringify(storedSave(save))); }
    catch (_) {dirty=true;lastSave=performance.now();return null;}
    dirty = false; lastSave = performance.now(); lastGeneration = core.emu_save_generation();
    syncStored();
    return save;
  }
  function savedTime(save) {
    const date=new Date(save.savedAt);
    return Number.isNaN(date.getTime())?'时间未记录':date.toLocaleString('zh-CN',{year:'numeric',month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit',second:'2-digit',hour12:false});
  }
  function node(tag,className,text) {
    const el=document.createElement(tag);el.className=className;if(text!==undefined)el.textContent=text;return el;
  }
  function stat(name,label,value) {
    const el=node('span','slot-stat');el.setAttribute('aria-label',`${label} ${value}`);
    const icon=node('i','stat-icon '+name);icon.setAttribute('aria-hidden','true');el.append(icon,node('span','',String(value)));return el;
  }
  function renderSlots() {
    const data=readSlots(),list=$('save-slots');list.replaceChildren();
    data.slots.forEach((save,index)=>{
      const card=node('button','slot-card');card.type='button';card.dataset.slot=String(index);
      card.disabled=slotMode==='load'&&!save;
      card.setAttribute('aria-label',`存档槽 ${index+1}${save?'，第 '+save.state.floor+' 层，血量 '+save.state.hp+'，攻击 '+save.state.atk+'，防御 '+save.state.def:'，空存档'}`);
      const heading=node('span','slot-heading');heading.append(node('span','',`存档槽 ${index+1}`));
      if(save){
        const s=save.state,floor=node('span','slot-floor');const icon=node('i','stat-icon floor');icon.setAttribute('aria-hidden','true');floor.append(icon,node('span','',`第 ${s.floor} 层`));heading.append(floor);card.append(heading);
        const stats=node('span','slot-stats');for(const [name,label] of [['hp','血量'],['atk','攻击'],['def','防御'],['gold','金币'],['yellow','黄钥匙'],['blue','蓝钥匙'],['red','红钥匙']])stats.append(stat(name,label,s[name]));
        card.append(stats,node('time','slot-time',savedTime(save)));
      } else {heading.append(node('span','slot-floor','空存档'));card.append(heading);}
      card.addEventListener('click',()=>{slotCursor=index;selectSlot();});
      card.addEventListener('focus',()=>{if(overwriteSlot===null){slotCursor=index;selectSlot();}});list.append(card);
    });
    const auto=readStored();$('save-auto').hidden=slotMode!=='load'||!auto;
    if(auto)$('save-auto').textContent=`继续自动进度 · 第 ${auto.state.floor} 层 · ${savedTime(auto)}`;
    selectSlot();
  }
  function slotChoices() {
    const cards=Array.from($('save-slots').children);
    return [...cards.map((card,i)=>card.disabled?null:i).filter(i=>i!==null),...($('save-auto').hidden?[]:[SLOT_COUNT])];
  }
  function selectSlot() {
    const choices=slotChoices();if(!choices.includes(slotCursor))slotCursor=choices[0]??0;
    Array.from($('save-slots').children).forEach((card,i)=>{
      if(i===slotCursor)card.classList.add('selected');else card.classList.remove('selected');
      card.setAttribute('aria-current',i===slotCursor?'true':'false');
    });
    if(slotCursor===SLOT_COUNT)$('save-auto').classList.add('selected');else $('save-auto').classList.remove('selected');
    $('save-slot-action').disabled=slotCursor===SLOT_COUNT || !choices.includes(slotCursor);
    $('save-slot-action').textContent=(slotMode==='load'?'读取':'保存到')+`存档槽 ${Math.min(slotCursor+1,SLOT_COUNT)}`;
    const selected=slotCursor===SLOT_COUNT?$('save-auto'):$('save-slots').children[slotCursor];
    selected?.scrollIntoView?.({block:'nearest'});
  }
  function cancelOverwrite() {
    overwriteSlot=null;$('save-overwrite').hidden=true;selectSlot();
  }
  function slotKey(key) {
    if(key===27){if(overwriteSlot!==null)cancelOverwrite();else closeSlots();return;}
    if(overwriteSlot!==null){
      if([37,38,39,40].includes(key)){
        overwriteConfirm=!overwriteConfirm;
        $(overwriteConfirm?'save-overwrite-confirm':'save-overwrite-cancel').focus();
      } else if(key===41){if(overwriteConfirm)chooseSlot(overwriteSlot,true);else cancelOverwrite();}
      return;
    }
    if([37,38,39,40].includes(key)){
      const choices=slotChoices(),step=key===37||key===38?-1:1;
      slotCursor=choices[(choices.indexOf(slotCursor)+step+choices.length)%choices.length]??0;
      selectSlot();const target=slotCursor===SLOT_COUNT?$('save-auto'):$('save-slots').children[slotCursor];
      target?.focus({preventScroll:true});target?.scrollIntoView({block:'nearest'});
    } else if(key===41){if(slotCursor===SLOT_COUNT)continueAuto();else chooseSlot(slotCursor);}
  }
  function continueAuto() {
    const save=readStored();if(!save)return;if(dirty)checkpoint();closeSlots();load(save);
  }
  function openSlots(mode,save=null) {
    if(!core||!gameReady)return;
    if(mode==='save'){save=captureSave();if(!save)return;}
    pickerReturn={menu:!$('menu').hidden,paused:!$('paused').hidden,wasPaused:paused};
    slotMode=mode;pendingSave=save;overwriteSlot=null;slotCursor=readSlots().selected??0;paused=true;release();
    $('menu').hidden=true;$('paused').hidden=true;$('save-picker').hidden=false;
    $('save-picker-title').textContent=mode==='load'?'读取进度':mode==='import'?'导入存档':'保存进度';
    $('save-picker-subtitle').textContent=mode==='load'?'3 个存档槽':`第 ${save.state.floor} 层 · 选择存档槽`;
    $('save-overwrite').hidden=true;$('save-picker-error').hidden=true;renderSlots();updateTools();
    const selected=slotCursor===SLOT_COUNT?$('save-auto'):$('save-slots').children[slotCursor];selected?.focus({preventScroll:true});
  }
  function closeSlots() {
    if(!slotMode)return;
    const mode=slotMode,previous=pickerReturn;slotMode=null;pendingSave=null;overwriteSlot=null;pickerReturn=null;
    $('save-picker').hidden=true;release();paused=previous.wasPaused;$('menu').hidden=!previous.menu;$('paused').hidden=!previous.paused;
    last=performance.now();syncStored();
    if(previous.menu)$('menu-save').focus();else focusGame();
  }
  function slotError(err) {
    if(!slotMode){error(err);return;}
    $('save-picker-error').textContent=err.message || '无法保存，请检查浏览器存储空间';$('save-picker-error').hidden=false;
  }
  function chooseSlot(index,confirmed=false) {
    try {
      const data=readSlots(),save=data.slots[index];if(slotMode==='load'){
        if(!save)return;if(dirty)checkpoint();try{writeSlots({...data,selected:index});}catch(_){}closeSlots();load(save);return;
      }
      if(!pendingSave)return;
      if(save&&!confirmed){overwriteSlot=index;overwriteConfirm=false;$('save-overwrite-text').textContent=`覆盖存档槽 ${index+1}（第 ${save.state.floor} 层）？`;$('save-overwrite').hidden=false;$('save-overwrite-cancel').focus();return;}
      data.slots[index]=validate(SaveCrypto.seal(saveBytes(pendingSave),new Date().toISOString()));data.selected=index;writeSlots(data);
      const imported=slotMode==='import',saved=data.slots[index];closeSlots();if(imported)load(saved);checkpoint();
    } catch(err){slotError(err);}
  }
  function load(save) {
    const valid = validate(save), bytes = saveBytes(valid);
    release(); core.emu_init(); check();
    const pointer = core.web_scratch();
    new Uint8Array(core.memory.buffer,pointer,bytes.length).set(bytes);
    if(!core.emu_save_import(pointer,bytes.length)) throw new Error('读取存档失败');
    core.emu_key(40); core.emu_key(41); refresh();
    timers.clear(); dirty = false; lastGeneration = core.emu_save_generation(); last = performance.now();
    setPaused(false);
  }
  function setMenu(value) {
    if(slotMode)return;
    paused = value; release(); $('menu').hidden = !value; $('paused').hidden = true;$('menu-error').hidden=true;
    last = performance.now();
    updateTools();
    if(value)$('menu-resume').focus({preventScroll:true});else focusGame();
  }
  function setPaused(value) {
    if(slotMode)closeSlots();
    paused = value; release(); $('menu').hidden = true; $('paused').hidden = !value;
    last = performance.now();
    updateTools();
    if(!value)focusGame();
  }
  function frame(now) {
    if(slotMode && !document.hidden && held!==null && now>=repeatAt){act(held);repeatAt=now+180;}
    if (core && !paused && !document.hidden) {
      const elapsed = Math.min(Math.max(now-last,0),100);
      try {
        const wasBusy = core.web_busy(), tickResult = core.web_tick(Math.round(elapsed));
        if(tickResult===2 || wasBusy && !core.web_busy()) { refresh(); dirty = true; }
        // Keep the physical hold during combat, but never send movement into
        // the native attack timer. Releasing or sliding still updates it.
        if (held !== null && now >= repeatAt && !core.web_combat_active() && !core.web_busy()) {
          act(held); repeatAt = now + 110;
        }
        const active = new Set();
        for(let i=0;i<16;i++) {
          const id = core.web_timer_id(i); if(!id) break;
          active.add(id);
          const interval = Math.max(5,core.web_timer_interval(i)*5);
          let debt = (timers.get(id)||0) + elapsed;
          if(debt >= interval) { core.emu_timer(id); refresh(); debt %= interval; }
          timers.set(id,debt);
        }
        for(const id of timers.keys()) if(!active.has(id)) timers.delete(id);
        if(dirty && now-lastSave>2500) checkpoint();
      } catch (err) { error(err); }
    }
    updateTools(); last = now; requestAnimationFrame(frame);
  }
  document.querySelectorAll('[data-key]').forEach(button => {
    button.disabled = true;
    if(joystick.contains(button)) {
      button.addEventListener('click',event => { if(event.detail===0) act(Number(button.dataset.key)); });
      return;
    }
    button.addEventListener('pointerdown', event => {
      if(button.disabled) return;
      event.preventDefault(); button.setPointerCapture(event.pointerId); hold(Number(button.dataset.key),button);
    });
    button.addEventListener('pointerup',release); button.addEventListener('pointercancel',release);
    button.addEventListener('lostpointercapture',release);
    // Keyboard activation and assistive technology emit click without a pointer.
    button.addEventListener('click',event => { if(event.detail===0) act(Number(button.dataset.key)); });
  });
  function joystickDirection(event) {
    // Four circular sectors meet along the diagonals, with a neutral center.
    const rect = joystick.getBoundingClientRect();
    const dx = event.clientX-rect.left-rect.width/2, dy = event.clientY-rect.top-rect.height/2;
    const radius = Math.min(rect.width,rect.height)/2, distance = Math.hypot(dx,dy);
    const key = distance<=radius*20/72 || distance>radius ? null :
      Math.abs(dx)>Math.abs(dy) ? (dx<0?37:39) : (dy<0?38:40);
    const button = directionButtons.find(button => Number(button.dataset.key)===key);
    if(key===held) return;
    held = null;
    joystick.querySelectorAll('.pressed').forEach(button => button.classList.remove('pressed'));
    if(!button || button.disabled) return;
    act(key);
    if(!slotMode && (paused || core.web_busy())) { release(); return; }
    held = key; repeatAt = performance.now()+300;
    button.classList.add('pressed');
  }
  joystick.addEventListener('pointerdown',event => {
    if(!core || !gameReady || !slotMode && (paused || core.web_busy()) || joystickPointer!==null || !event.isPrimary) return;
    event.preventDefault(); release(); joystickPointer=event.pointerId;
    joystick.setPointerCapture(event.pointerId); joystickDirection(event);
  });
  joystick.addEventListener('pointermove',event => {
    if(event.pointerId!==joystickPointer) return;
    event.preventDefault(); joystickDirection(event);
  });
  for(const name of ['pointerup','pointercancel','lostpointercapture']) {
    joystick.addEventListener(name,event => { if(event.pointerId===joystickPointer) release(); });
  }
  joystick.addEventListener('contextmenu',event => event.preventDefault());
  document.addEventListener('keydown',event => {
    if(event.ctrlKey || event.altKey || event.metaKey || event.target.tagName==='INPUT' ||
       event.target.tagName==='TEXTAREA' || event.target.isContentEditable) return;
    const key = keyboardKey(event);
    const digit=/^(?:Digit|Numpad)([0-9])$/.exec(event.code||'')?.[1] ?? (/^[0-9]$/.test(event.key)?event.key:null);
    if(digit!==null){event.preventDefault();if(!event.repeat&&!event.shiftKey)useItem((Number(digit)+9)%10);return;}
    // Hardware shortcuts never repeat or bypass the corresponding button state.
    const shortcut = {F5:'save',F9:'continue',PageUp:'fly-up',PageDown:'fly-down'}[event.key];
    if(shortcut){
      event.preventDefault();
      if(core && gameReady && !slotMode && !event.repeat && !$(shortcut).disabled){release();$(shortcut).click();}
      return;
    }
    if(paused && !slotMode && !$('menu').hidden && [37,38,39,40].includes(key)){
      event.preventDefault();release();
      const buttons=Array.from($('menu').querySelectorAll('button:not([disabled])'));
      const index=buttons.indexOf(document.activeElement),step=key===37||key===38?-1:1;
      buttons[(index+step+buttons.length)%buttons.length]?.focus({preventScroll:true});return;
    }
    if(paused && !slotMode && !$('menu').hidden && key===41 && !['Enter',' '].includes(event.key)){
      event.preventDefault();if(!event.repeat)document.activeElement?.click();return;
    }
    // Let focused buttons keep their native Enter/Space activation. Otherwise
    // Enter on Close could load a slot, or Enter on Save could reach the game.
    if(['Enter',' '].includes(event.key) && event.target.matches?.('button:not(.slot-card)')) return;
    if(slotMode){
      if(key!==undefined){event.preventDefault();if(!event.repeat)hold(key,directionButtons.find(button=>Number(button.dataset.key)===key));}
      else if(event.key==='Tab'){
        const buttons=Array.from($('save-picker').querySelectorAll('button:not([disabled])')).filter(b=>b.getClientRects().length);
        const first=buttons[0],end=buttons[buttons.length-1];
        if(event.shiftKey&&document.activeElement===first){event.preventDefault();end.focus();}
        else if(!event.shiftKey&&document.activeElement===end){event.preventDefault();first.focus();}
      }
      return;
    }
    if(key===undefined) return;
    event.preventDefault(); if(!event.repeat){if(!paused)focusGame();hold(key,directionButtons.find(button=>Number(button.dataset.key)===key));}
  });
  document.addEventListener('keyup',event => { if(keyboardKey(event)===held) release(); });
  window.addEventListener('blur',release);
  window.addEventListener('storage',event => {
    if(event.key===STORE || event.key===SLOT_STORE || event.key===null) {
      syncStored();if(slotMode){overwriteSlot=null;$('save-overwrite').hidden=true;renderSlots();}
    }
  });
  document.addEventListener('visibilitychange',() => {
    release(); last = performance.now();
    if(document.hidden && dirty) { try { checkpoint(); } catch(_) {} }
  });
  window.addEventListener('pagehide',() => { if(dirty) { try { checkpoint(); } catch(_) {} } });
  canvas.addEventListener('pointerdown',event => {
    if(!core || !gameReady || paused) return;
    event.preventDefault();
    focusGame();
    const bounds = canvas.getBoundingClientRect();
    const x = Math.min(239,Math.max(0,Math.floor((event.clientX-bounds.left)*240/bounds.width)));
    const y = viewTop + Math.min(canvas.height-1,Math.max(0,Math.floor((event.clientY-bounds.top)*canvas.height/bounds.height)));
    try { core.web_touch(x,y); refresh(); dirty = core.emu_mode()===0; } catch(err) { error(err); }
  });
  $('resume').addEventListener('click',() => setPaused(false));
  $('menu-resume').addEventListener('click',() => setMenu(false));
  $('menu-save').addEventListener('click',() => { try { openSlots('save'); } catch(err) { error(err); } });
  $('menu-title').addEventListener('click',() => {
    try {
      if(dirty && !checkpoint()){menuError('无法保存当前进度，请先保存到存档槽或导出存档');return;}
      const saved = readStored(); release(); core.emu_init(); check();
      if(saved) {
        const bytes=saveBytes(saved),pointer=core.web_scratch();
        new Uint8Array(core.memory.buffer,pointer,bytes.length).set(bytes);
        core.emu_save_import(pointer,bytes.length);
      }
      refresh(); timers.clear(); dirty=false; setPaused(false);
    } catch(err) { error(err); }
  });
  $('save').addEventListener('click',() => { try { openSlots('save'); } catch(err) { error(err); } });
  $('continue').addEventListener('click',()=>{syncStored();if(hasStoredSave)openSlots('load');});
  $('save-picker-close').addEventListener('click',closeSlots);
  $('save-slot-action').addEventListener('click',()=>chooseSlot(slotCursor));
  $('save-picker').addEventListener('click',event=>{if(event.target===$('save-picker'))closeSlots();});
  $('save-overwrite-cancel').addEventListener('click',cancelOverwrite);
  $('save-overwrite-confirm').addEventListener('click',()=>{if(overwriteSlot!==null)chooseSlot(overwriteSlot,true);});
  $('save-auto').addEventListener('click',()=>{try{continueAuto();}catch(err){error(err);}});
  $('save-overwrite-cancel').addEventListener('focus',()=>{overwriteConfirm=false;});
  $('save-overwrite-confirm').addEventListener('focus',()=>{overwriteConfirm=true;});
  function fly(down){
    if(!core || !gameReady || paused || slotMode || !core.web_can_fly(down))return;
    try{release();const before=core.emu_floor();core.web_fly(down);refresh();if(core.emu_floor()!==before)dirty=true;}
    catch(err){error(err);}
  }
  $('fly-up').addEventListener('click',()=>fly(0));
  $('fly-down').addEventListener('click',()=>fly(1));
  function useItem(slot){
    if(!core||!gameReady||paused||slotMode||!core.web_can_item(slot))return;
    try{release();focusGame();core.web_item_use(slot);refresh();dirty=true;}
    catch(err){error(err);}
  }
  itemButtons.forEach((button,slot)=>button.addEventListener('click',()=>useItem(slot)));
  $('export').addEventListener('click',() => {
    try {
      const data=readSlots(),current=captureSave();if(current)checkpoint(current);
      const save = current || readStored() || data.slots[data.selected] || data.slots.find(Boolean); if(!save) return;
      const url = URL.createObjectURL(new Blob([JSON.stringify(storedSave(save),null,2)],{type:'application/json'}));
      const a = document.createElement('a'); a.href=url; a.download=`魔塔50-第${save.state.floor}层.json`; a.click();
      setTimeout(() => URL.revokeObjectURL(url),1000);
    } catch(err) { menuError('无法导出：'+err.message);console.warn(err); }
  });
  $('import').addEventListener('change',async event => {
    const file = event.target.files[0]; if(!file) return;
    try {
      if(file.size>50000) throw new Error('存档文件过大');
      if(file.name.toLowerCase().endsWith('.bin'))throw new Error('只支持新版加密存档，旧版 BIN 文件不能直接导入');
      let save=JSON.parse(await file.text());
      save=validate(save);openSlots('import',save);
    } catch(err) {menuError('无法导入：'+err.message);console.warn(`无法导入：${err.message}`);}
    event.target.value='';
  });
  function loading(stage, detail='', progress=null) {
    $('loading-stage').textContent = stage; $('loading-detail').textContent = detail;
    if(progress===null) $('loading-progress').removeAttribute('value');
    else $('loading-progress').value = Math.min(100,Math.max(0,progress));
  }
  const yieldPaint = () => new Promise(resolve=>setTimeout(resolve,0));
  async function resourcePack() {
    if(window.MAGIC_TOWER_PACK) return window.MAGIC_TOWER_PACK;
    if(window.location.protocol==='file:') {
      loading('读取本地游戏资源','地图 · 贴图 · 字库 · 原版游戏');
      await new Promise((resolve,reject)=>{
        const script=document.createElement('script'); script.src=runtimePath;
        script.onload=resolve; script.onerror=()=>reject(new Error('无法读取游戏资源，请保留完整的游戏文件'));
        document.head.appendChild(script);
      });
      return window.MAGIC_TOWER_PACK;
    }
    loading('下载游戏资源','地图 · 贴图 · 字库 · 原版游戏',0);
    const response=await fetch(runtimePath);
    if(!response.ok) throw new Error(`资源下载失败（${response.status}），请重试`);
    const total=runtimeSize || (!response.headers.get('Content-Encoding') ? Number(response.headers.get('Content-Length')) : 0);
    const chunks=[]; let received=0;
    const show=()=>loading('下载游戏资源'+(total?' '+Math.min(100,Math.floor(received/total*100))+'%':''),
      '地图、贴图、字库、游戏程序\n已读取 '+(received/1048576).toFixed(2)+(total?' / '+(total/1048576).toFixed(2):'')+' MB 资源',total?received/total*100:null);
    if(response.body){
      const reader=response.body.getReader();
      while(true){const {value,done}=await reader.read();if(done)break;chunks.push(value);received+=value.byteLength;show();}
    } else {const value=new Uint8Array(await response.arrayBuffer());chunks.push(value);received=value.length;show();}
    if(total&&received!==total) throw new Error('游戏资源未下载完整，请重新加载');
    const text=await new Blob(chunks).text(),prefix='window.MAGIC_TOWER_PACK=';
    if(!text.startsWith(prefix)||!text.trimEnd().endsWith(';')) throw new Error('游戏资源格式不正确，请重新加载');
    return JSON.parse(text.slice(prefix.length).trim().slice(0,-1));
  }
  // Small dependency-free fallback for browsers without Compression Streams.
  // Decode gzip's stored, fixed and dynamic DEFLATE blocks, then check CRC/size.
  function inflateGzip(input,size) {
    const fail=()=>{throw new Error('游戏资源损坏，请重新加载');};
    if(input.length<18||input[0]!==31||input[1]!==139||input[2]!==8||(input[3]&224))fail();
    let pos=10,bit=0;const end=input.length-8,flags=input[3];
    if(flags&4){if(pos+2>end)fail();const n=input[pos]|input[pos+1]<<8;pos+=2+n;}
    for(const flag of [8,16])if(flags&flag){while(pos<end&&input[pos++]);if(pos>end)fail();}
    if(flags&2)pos+=2;if(pos>=end)fail();
    const bits=n=>{let value=0;for(let i=0;i<n;i++){if(pos>=end)fail();value|=((input[pos]>>>bit)&1)<<i;if(++bit===8){bit=0;pos++;}}return value;};
    const table=lengths=>{
      const count=Array(16).fill(0),next=Array(16).fill(0),maps=[];
      for(const n of lengths){if(n>15)fail();if(n)count[n]++;}
      let code=0;for(let n=1;n<=15;n++){code=(code+count[n-1])*2;if(code+count[n]>2**n)fail();next[n]=code;}
      lengths.forEach((n,symbol)=>{if(!n)return;let value=next[n]++,reversed=0;for(let i=0;i<n;i++){reversed=(reversed<<1)|(value&1);value>>>=1;}(maps[n]||=new Map()).set(reversed,symbol);});
      return maps;
    };
    const symbol=maps=>{let code=0;for(let n=1;n<=15;n++){code|=bits(1)<<(n-1);const s=maps[n]?.get(code);if(s!==undefined)return s;}fail();};
    const lengthBase=[3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258];
    const lengthExtra=[0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0];
    const distanceBase=[1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577];
    const distanceExtra=[0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13];
    const output=new Uint8Array(size);let written=0,final=0;
    while(!final){
      final=bits(1);const type=bits(2);
      if(type===0){
        if(bit){pos++;bit=0;}if(pos+4>end)fail();const n=input[pos]|input[pos+1]<<8,inv=input[pos+2]|input[pos+3]<<8;pos+=4;
        if((n^inv)!==65535||pos+n>end||written+n>size)fail();output.set(input.subarray(pos,pos+n),written);pos+=n;written+=n;continue;
      }
      let literal,distance;
      if(type===1){literal=table(Array.from({length:288},(_,i)=>i<144?8:i<256?9:i<280?7:8));distance=table(Array(32).fill(5));}
      else if(type===2){
        const nl=bits(5)+257,nd=bits(5)+1,nc=bits(4)+4;
        // RFC 1951's code-length alphabet order.
        const alphabet=[16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15],lens=Array(19).fill(0);
        for(let i=0;i<nc;i++)lens[alphabet[i]]=bits(3);
        const codes=table(lens),lengths=[];
        while(lengths.length<nl+nd){const s=symbol(codes);if(s<16)lengths.push(s);else{
          if(s===16&&!lengths.length)fail();const value=s===16?lengths[lengths.length-1]:0,n=s===16?bits(2)+3:s===17?bits(3)+3:bits(7)+11;
          if(lengths.length+n>nl+nd)fail();for(let i=0;i<n;i++)lengths.push(value);
        }}
        if(!lengths[256])fail();literal=table(lengths.slice(0,nl));distance=table(lengths.slice(nl));
      } else fail();
      while(true){const s=symbol(literal);if(s===256)break;
        if(s<256){if(written>=size)fail();output[written++]=s;continue;}
        const index=s-257;if(index>=lengthBase.length)fail();const n=lengthBase[index]+bits(lengthExtra[index]),d=symbol(distance);
        if(d>=distanceBase.length)fail();const offset=distanceBase[d]+bits(distanceExtra[d]);if(offset>written||written+n>size)fail();
        for(let i=0;i<n;i++){output[written]=output[written-offset];written++;}
      }
    }
    if(written!==size)fail();const trailer=new DataView(input.buffer,input.byteOffset+end,8);
    if(trailer.getUint32(4,true)!==size)fail();
    const crcTable=new Uint32Array(256);for(let i=0;i<256;i++){let c=i;for(let j=0;j<8;j++)c=c&1?0xedb88320^(c>>>1):c>>>1;crcTable[i]=c;}
    let crc=0xffffffff;for(const byte of output)crc=crcTable[(crc^byte)&255]^(crc>>>8);
    if(((crc^0xffffffff)>>>0)!==trailer.getUint32(0,true))fail();return output;
  }
  async function gameBytes() {
    if(window.MAGIC_TOWER_WASM){const bytes=decode(window.MAGIC_TOWER_WASM);delete window.MAGIC_TOWER_WASM;return bytes;}
    const pack=await resourcePack();delete window.MAGIC_TOWER_PACK;
    if(!pack||pack.format!=='gzip'||!Number.isInteger(pack.bytes)||pack.bytes<8||pack.bytes>16777216||typeof pack.data!=='string')throw new Error('游戏资源格式不正确');
    loading('解压游戏资源','正在准备地图、贴图、字库和原版程序');await yieldPaint();
    const compressed=decode(pack.data);
    let bytes;
    try {
      bytes=typeof DecompressionStream==='function'?
        new Uint8Array(await new Response(new Blob([compressed]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer()):inflateGzip(compressed,pack.bytes);
    } catch (_) { throw new Error('游戏资源解压失败，请重新加载'); }
    if(bytes.length!==pack.bytes)throw new Error('游戏资源未解压完整，请重新加载');return bytes;
  }
  $('loading-retry').addEventListener('click',()=>window.location.reload());
  async function boot() {
    try {
      const bytes=await gameBytes();
      loading('启动原版游戏','正在初始化游戏画面');await yieldPaint();
      const {instance} = await WebAssembly.instantiate(bytes);core = instance.exports;
      if(!core.emu_init()) throw new Error('魔塔初始化失败'); refresh();
      loading('读取本地存档','正在检查已保存的进度');await yieldPaint();
      migrateEncryptedSaves();migrateSlots();const slots=readSlots(),saved = readStored() || slots.slots[slots.selected] || slots.slots.find(Boolean);
      hasStoredSave = !!saved;
      if(saved) {
        const bytes=saveBytes(saved),pointer=core.web_scratch();
        new Uint8Array(core.memory.buffer,pointer,bytes.length).set(bytes);
        core.emu_save_import(pointer,bytes.length);
      }
      gameReady = true; $('loading').hidden = true;
      document.querySelectorAll('button,input').forEach(button => button.disabled=false);
      updateTools();
      last = performance.now(); requestAnimationFrame(frame);
    } catch(err) {
      gameReady=false;core=undefined;
      document.querySelectorAll('button,input').forEach(button=>button.disabled=true);
      $('loading').querySelector('strong').textContent='游戏加载失败';
      $('loading').classList.add('failed');loading('暂时无法打开游戏',err.message);
      $('loading-retry').hidden=false;$('loading-retry').disabled=false;
    }
  }
  boot();
})();

// Private fixtures: exercise the real codec without exposing it in the Web API.
const fs=require('node:fs'),vm=require('node:vm');
const scope={Uint8Array,Uint32Array,DataView,ArrayBuffer,Date,Math,JSON,Error,
  crypto:require('node:crypto').webcrypto,
  atob:s=>Buffer.from(s,'base64').toString('binary'),btoa:s=>Buffer.from(s,'binary').toString('base64')};
vm.runInNewContext(fs.readFileSync(require('node:path').join(__dirname,'save-crypto.bundle.js'),'utf8'),scope);
const codec=scope.SaveCrypto;
const seal=save=>save.version===2?codec.envelope(save):codec.seal(new Uint8Array(Buffer.from(save.data,'base64')),save.savedAt);
function open(save){
  if(!save||save.version!==2)return save;
  const {bytes,savedAt}=codec.open(save),b=Buffer.from(bytes),word=offset=>b.readUInt32LE(offset);
  return {format:save.format,version:1,savedAt,data:b.toString('base64'),state:{floor:word(5),x:word(0x76),y:word(0x7a),hp:word(0x46),atk:word(0x4a),def:word(0x4e),gold:word(0x52),yellow:word(0x5a),blue:word(0x5e),red:word(0x62)}};
}
module.exports={codec,seal,open};

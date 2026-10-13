const projectRoot = require('./setup.cjs');
const fs=require('node:fs'),assert=require('node:assert/strict'),vm=require('node:vm');
const {codec}=require('./test-save-helpers.cjs');
const bytes=new Uint8Array(6304);for(let i=0;i<bytes.length;i++)bytes[i]=(i*17)&255;
const when='2026-10-13T00:00:00.000Z',save=codec.seal(bytes,when),other=codec.seal(bytes,when);
assert.notEqual(save.nonce,other.nonce);assert.notEqual(save.data,other.data);
assert.deepEqual(Buffer.from(codec.open(save).bytes),Buffer.from(bytes));assert.equal(codec.open(save).savedAt,when);
assert.deepEqual(Object.keys(save).sort(),['cipher','data','format','nonce','version']);
const raw=Buffer.from(save.data,'base64');assert.notDeepEqual(raw.subarray(0,6304),Buffer.from(bytes));
for(const offset of [0,7,40,500,6327]){
 const changed=Buffer.from(raw);changed[offset]^=1;
 assert.throws(()=>codec.open({...save,data:changed.toString('base64')}),/校验失败/);
}
const nonce=Buffer.from(save.nonce,'base64');nonce[0]^=1;
assert.throws(()=>codec.open({...save,nonce:nonce.toString('base64')}),/校验失败/);
assert.throws(()=>codec.open({...save,data:save.data.slice(0,-4)}),/校验失败/);
assert.throws(()=>codec.open({...save,version:1}),/新版加密/);
assert.throws(()=>codec.open({...save,cipher:'none'}),/新版加密/);
assert.throws(()=>codec.open({format:save.format,version:1,data:Buffer.from(bytes).toString('base64')}),/新版加密/);
assert.equal(codec.open({...save,savedAt:'2099-01-01',state:{hp:999999}}).savedAt,when);
const isolated={Uint8Array,Uint32Array,DataView,ArrayBuffer,Date,JSON,Math,Error,
 crypto:{getRandomValues:a=>require('node:crypto').webcrypto.getRandomValues(a)},
 atob:s=>Buffer.from(s,'base64').toString('binary'),btoa:s=>Buffer.from(s,'binary').toString('base64')};
vm.runInNewContext(fs.readFileSync('save-crypto.bundle.js','utf8'),isolated);
assert.deepEqual(Buffer.from(isolated.SaveCrypto.open(save).bytes),Buffer.from(bytes));
assert.deepEqual(Buffer.from(isolated.SaveCrypto.open(isolated.SaveCrypto.seal(bytes,when)).bytes),Buffer.from(bytes));
const report={result:'PASS',checks:['unique CSPRNG nonces and ciphertext','exact binary and authenticated timestamp round trip','no clear attribute or timestamp metadata in persisted envelope','ciphertext, nonce, tag, version, cipher and truncation tampering refused','legacy plaintext envelope rejected','metadata injection ignored','fresh-browser codec reads existing encrypted exports','HTTP-compatible encryption without WebCrypto subtle']};
fs.writeFileSync('verification/save-crypto.json',JSON.stringify(report,null,2));console.log(JSON.stringify(report,null,2));

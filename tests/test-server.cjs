const projectRoot = require('./setup.cjs');
const fs=require('node:fs'),assert=require('node:assert/strict'),http=require('node:http'),net=require('node:net'),{spawn}=require('node:child_process');
(async()=>{
 const probe=net.createServer();await new Promise(r=>probe.listen(0,'127.0.0.1',r));const port=probe.address().port;await new Promise(r=>probe.close(r));
 const child=spawn(process.execPath,['serve.cjs'],{env:{...process.env,PORT:String(port)},windowsHide:true,stdio:['ignore','pipe','pipe']});
 try{
  await new Promise((r,j)=>{child.stdout.once('data',r);child.once('error',j);child.once('exit',c=>j(new Error('server exited '+c)));});
  const request=(url,method='GET')=>new Promise((r,j)=>{http.request({hostname:'127.0.0.1',port,path:url,method},res=>{const chunks=[];res.on('data',c=>chunks.push(c));res.on('end',()=>r({status:res.statusCode,body:Buffer.concat(chunks),headers:res.headers}));}).on('error',j).end();});
  for(const path of ['/','/index.html','/style.css','/game.js','/game-data.js?v=test'])assert.equal((await request(path)).status,200);
  for(const path of ['/game.wasm','/assets/fw.bin','/verification/story-dialog-fixture.json','/deploy.py','/native/emu_core.c','/serve.cjs','/AGENTS.md','/game-source.js','/save-crypto.mjs','/save-key.json','/package-lock.json','/node_modules/@noble/ciphers/chacha.js','/..%2f../magic-tower-web/deploy.py','/game.js%00'])assert.equal((await request(path)).status,404,path+' must be private');
  assert.equal((await request('/%ZZ')).status,400);assert.equal((await request('/game.js','POST')).status,405);const head=await request('/game.js','HEAD');assert.equal(head.status,200);assert.equal(head.body.length,0);
  const report={result:'PASS',checks:['four runtime files and root page served','firmware, source, saves, deployment script and private tests return 404','encoded traversal/null paths refused','HEAD, unsupported methods and malformed URL handled']};fs.writeFileSync('verification/server.json',JSON.stringify(report,null,2));console.log(JSON.stringify(report,null,2));
 }finally{child.kill();}
})().catch(e=>{console.error(e);process.exitCode=1;});

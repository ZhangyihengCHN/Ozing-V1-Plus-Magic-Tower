// Optional static server for phone testing. Opening index.html also works offline.
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const root = __dirname;
const types = {'.html':'text/html; charset=utf-8','.css':'text/css; charset=utf-8',
  '.js':'text/javascript; charset=utf-8','.wasm':'application/wasm','.json':'application/json'};
const publicFiles=new Set(['index.html','style.css','game.js','game-data.js']);
const port = Number(process.env.PORT || 8765);
http.createServer((req,res) => {
  let pathname;
  try { pathname = decodeURIComponent(new URL(req.url,'http://localhost').pathname); }
  catch (_) { res.writeHead(400); return res.end('Bad request'); }
  const name=pathname==='/'?'index.html':pathname.slice(1);
  if(!publicFiles.has(name)) { res.writeHead(404); return res.end('Not found'); }
  const file = path.resolve(root,name);
  if(!file.startsWith(root+path.sep)) { res.writeHead(403); return res.end('Forbidden'); }
  if(req.method!=='GET'&&req.method!=='HEAD') { res.writeHead(405); return res.end(); }
  fs.stat(file,(err,stat) => {
    if(err || !stat.isFile()) { res.writeHead(404); return res.end('Not found'); }
    res.writeHead(200,{'Content-Type':types[path.extname(file)] || 'application/octet-stream',
      'Content-Length':stat.size,'Cache-Control':'no-cache'});
    if(req.method==='HEAD') return res.end();
    const stream=fs.createReadStream(file);stream.on('error',()=>res.destroy());stream.pipe(res);
  });
}).listen(port,'0.0.0.0',() => console.log(`Magic Tower: http://localhost:${port} (Ctrl+C to stop)`));

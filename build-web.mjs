import fs from 'node:fs/promises';
import { randomBytes } from 'node:crypto';
import { build } from 'esbuild';

const root = new URL('.', import.meta.url);
const keyPath = new URL('save-key.json', root);
try { await fs.access(keyPath); }
catch (_) { await fs.writeFile(keyPath, JSON.stringify([...randomBytes(32)]) + '\n', { flag: 'wx' }); }
const key = JSON.parse(await fs.readFile(keyPath, 'utf8'));
if (!Array.isArray(key) || key.length !== 32 || key.some(byte => !Number.isInteger(byte) || byte < 0 || byte > 255)) {
  throw new Error('save-key.json must contain exactly 32 bytes; preserve the existing key when updating');
}
const bundled = await build({
  entryPoints: [new URL('save-crypto.mjs', root).pathname.replace(/^\/([A-Za-z]:)/, '$1')],
  bundle: true, write: false, format: 'iife', globalName: 'SaveCrypto',
  minify: true, platform: 'browser', target: 'es2020', legalComments: 'inline'
});
const crypto = bundled.outputFiles[0].text;
await fs.writeFile(new URL('save-crypto.bundle.js', root), crypto);
const source = await fs.readFile(new URL('game-source.js', root), 'utf8');
const license = await fs.readFile(new URL('node_modules/@noble/ciphers/LICENSE', root), 'utf8');
const banner = '/* Bundled @noble/ciphers 2.4.0\n' + license.replaceAll('*/', '* /') + '\n*/\n';
await fs.writeFile(new URL('game.js', root), banner + '(() => {\n' + crypto + '\n' + source + '\n})();\n');
console.log('Built offline save encryption into game.js (no extra runtime requests)');

// Audit tracked content only; original resources and build outputs must stay local.
import fs from 'node:fs/promises';
import { execFileSync } from 'node:child_process';

const names = execFileSync('git', ['ls-files', '-z'], { encoding: 'utf8', windowsHide: true }).split('\0').filter(Boolean);
if (!names.length) throw new Error('No tracked source files found');
const allowed = name => /^(native\/.*\.(?:c|h|inc|s)|tools\/.*\.mjs|\.github\/workflows\/.*\.yml|docker\/nginx\.conf|assets\/README\.md|tests\/(?:test-[\w-]+|setup)\.cjs)$/.test(name) || [
  '.gitignore', '.dockerignore', '.gitattributes', 'LICENSE', 'README.md', 'THIRD_PARTY_NOTICES.md', 'docs/github-actions.yml', 'docs/DEVELOPMENT.md',
  'CONTRIBUTING.md', 'SECURITY.md', 'Dockerfile', 'compose.yaml', 'index.html', 'style.css',
  'game-source.js', 'save-crypto.mjs', 'build-web.mjs', 'build.py', 'prepare-assets.py',
  'prepare_assets.py', 'package.py', 'extraction.json', 'package.json', 'package-lock.json', 'serve.cjs'
].includes(name);
for (const name of names) {
  if (!allowed(name)) throw new Error(`File outside the public source allowlist: ${name}`);
  const text = await fs.readFile(name, 'utf8');
  for (const pattern of [ /-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----/,
    /gh[pousr]_[A-Za-z0-9_]{20,}/, /github_pat_[A-Za-z0-9_]{20,}/,
    /(?:\b[A-Z]:[\\/](?:Users|hjxemu)[\\/])/i,
    /(?:\broot@\d{1,3}(?:\.\d{1,3}){3}\b)/
  ]) if (pattern.test(text)) throw new Error(`Private credential or machine path detected: ${name}`);
}
console.log(`PASS: ${names.length} tracked source files; no binary resources, generated runtime, save key or personal data`);

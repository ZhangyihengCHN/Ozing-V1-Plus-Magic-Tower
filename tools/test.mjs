import fs from 'node:fs/promises';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
await fs.mkdir(new URL('../verification/', import.meta.url), { recursive: true });
const tests = process.argv.includes('--crypto-only') ? ['save-crypto'] : [
  'core', 'prison-wake', 'basement', 'quick-items', 'item-limits', 'notebook',
  'flight', 'story', 'shop', 'display', 'notice', 'save-validation',
  'save-crypto', 'save-security', 'save-slots', 'joystick', 'loading', 'server'
];
for (const name of tests) {
  console.log(`\nRunning ${name}`);
  const script = fileURLToPath(new URL(`../tests/test-${name}.cjs`, import.meta.url));
  const result = spawnSync(process.execPath, [script], { cwd: root, stdio: 'inherit', windowsHide: true });
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status || 1);
}
console.log(`\n${tests.length} regression suites passed`);

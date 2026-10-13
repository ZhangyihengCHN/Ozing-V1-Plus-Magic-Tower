import fs from 'node:fs/promises';
import { spawnSync } from 'node:child_process';

await fs.mkdir('verification', { recursive: true });
const tests = process.argv.includes('--crypto-only') ? ['save-crypto'] : [
  'core', 'prison-wake', 'basement', 'quick-items', 'item-limits', 'notebook',
  'flight', 'story', 'shop', 'display', 'notice', 'save-validation',
  'save-crypto', 'save-security', 'save-slots', 'joystick', 'loading', 'server'
];
for (const name of tests) {
  console.log(`\nRunning ${name}`);
  const result = spawnSync(process.execPath, [`test-${name}.cjs`], { stdio: 'inherit', windowsHide: true });
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status || 1);
}
console.log(`\n${tests.length} regression suites passed`);

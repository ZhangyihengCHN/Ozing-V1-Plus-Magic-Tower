// Keep input resources and generated reports at the project root.
const fs = require('node:fs');
const root = require('node:path').resolve(__dirname, '..');
process.chdir(root);
fs.mkdirSync(require('node:path').join(root, 'verification'), { recursive: true });
module.exports = root;

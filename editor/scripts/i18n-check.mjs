// Lists t('...') keys used in src/ that are missing from the German pack
// (src/i18n/de.ts) and keys in the pack that are no longer used.
// Usage: node scripts/i18n-check.mjs [--json]  (exit code 1 when keys are missing)
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { tmpdir } from 'node:os';
import { build } from 'esbuild';

const root = join(dirname(fileURLToPath(import.meta.url)), '..', 'src');
const files = [];
const walk = (d) => { for (const f of readdirSync(d)) { const p = join(d, f); if (statSync(p).isDirectory()) walk(p); else if (/\.(ts|tsx)$/.test(f) && !p.endsWith('de.ts')) files.push(p); } };
walk(root);

const used = new Set();
const re = /\b(?:t|msg)\(\s*(['"`])((?:\\.|(?!\1).)*)\1/g;
for (const f of files) {
  const src = readFileSync(f, 'utf8');
  for (const m of src.matchAll(re)) if (!m[2].includes('${')) used.add(m[2].replace(/\\(['"`\\])/g, '$1'));
}
// Strings from data tables (class descriptions, prefab names, materials, …).
const out = join(tmpdir(), `forge-i18n-${process.pid}.mjs`);
await build({
  entryPoints: [join(root, '..', 'scripts', 'i18n-dynamic.ts')], bundle: true, platform: 'node', format: 'esm', outfile: out, logLevel: 'error',
  plugins: [{ name: 'stub-url', setup(b) { b.onResolve({ filter: /\?url$/ }, (a) => ({ path: a.path, namespace: 'stub' })); b.onLoad({ filter: /.*/, namespace: 'stub' }, () => ({ contents: 'export default ""' })); } }],
});
const { execFileSync } = await import('node:child_process');
for (const k of JSON.parse(execFileSync(process.execPath, [out], { encoding: 'utf8' }))) used.add(k);

const de = readFileSync(join(root, 'i18n', 'de.ts'), 'utf8');
const have = new Set();
for (const m of de.matchAll(/^\s*(['"])((?:\\.|(?!\1).)*)\1\s*:/gm)) have.add(m[2].replace(/\\(['"\\])/g, '$1'));
const missing = [...used].filter((k) => !have.has(k)).sort();
const unused = [...have].filter((k) => !used.has(k)).sort();
if (process.argv.includes('--json')) console.log(JSON.stringify({ missing, unused, used: [...used].sort() }, null, 1));
else {
  console.log(`${used.size} keys used, ${have.size} translated, ${missing.length} missing, ${unused.length} unused`);
  for (const k of missing) console.log('  missing: ' + k);
  for (const k of unused) console.log('  unused:  ' + k);
}
process.exit(missing.length ? 1 : 0);

// Builds the Rust core (engine/) to WebAssembly and copies it to src/engine/.
// If no Rust toolchain is installed, the committed prebuilt .wasm is used.
import { spawnSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync } from 'node:fs';
import { homedir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const out = join(root, 'src', 'engine', 'forge_core.wasm');
const exe = process.platform === 'win32' ? 'cargo.exe' : 'cargo';
const candidates = [join(homedir(), '.cargo', 'bin', exe), 'cargo'];
const cargo = candidates.find((c) => c === 'cargo' ? spawnSync('cargo', ['--version']).status === 0 : existsSync(c));

if (!cargo) {
  if (existsSync(out)) { console.log('[engine] Rust toolchain not found – using prebuilt src/engine/forge_core.wasm'); process.exit(0); }
  console.error('[engine] Rust toolchain not found and no prebuilt wasm present. Install Rust (rustup) and run: rustup target add wasm32-unknown-unknown');
  process.exit(1);
}

const run = (args) => {
  const r = spawnSync(cargo, args, { cwd: join(root, 'engine'), stdio: 'inherit' });
  if (r.status !== 0) process.exit(r.status ?? 1);
};
if (process.argv.includes('--test')) run(['test', '--release']);
run(['build', '--release', '--target', 'wasm32-unknown-unknown']);
mkdirSync(dirname(out), { recursive: true });
copyFileSync(join(root, 'engine', 'target', 'wasm32-unknown-unknown', 'release', 'forge_core.wasm'), out);
console.log('[engine] built src/engine/forge_core.wasm');

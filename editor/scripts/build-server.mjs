// Builds the Rust game server (server/) in release mode:
// server/target/release/forge-server(.exe). Studio's "Start Local Server"
// and `npm run server` use this binary.
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { homedir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const exe = process.platform === 'win32' ? 'cargo.exe' : 'cargo';
const candidates = [join(homedir(), '.cargo', 'bin', exe), 'cargo'];
const cargo = candidates.find((c) => c === 'cargo' ? spawnSync('cargo', ['--version']).status === 0 : existsSync(c));
if (!cargo) { console.error('[server] Rust toolchain not found. Install Rust (rustup) to build forge-server.'); process.exit(1); }

const args = process.argv.includes('--test') ? ['test', '--release'] : ['build', '--release'];
const r = spawnSync(cargo, args, { cwd: join(root, 'server'), stdio: 'inherit' });
process.exit(r.status ?? 1);

// Builds the Linux packages from Windows using WSL (Windows Subsystem for Linux).
// Usage: npm run dist:linux [-- studio player server]
//   Optional: set FORGE_WSL_DISTRO to pick a distribution (default: the WSL default).
// On a Linux machine, use `npm run dist` instead.
import { spawnSync } from 'node:child_process';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
if (process.platform !== 'win32') {
  console.error('[dist:linux] this helper is for Windows (WSL). On Linux run: npm run dist');
  process.exit(1);
}
const distro = process.env.FORGE_WSL_DISTRO ? ['-d', process.env.FORGE_WSL_DISTRO] : [];
const toWsl = (p) => '/mnt/' + p[0].toLowerCase() + p.slice(2).replace(/\\/g, '/');
const targets = process.argv.slice(2).filter((a) => !a.startsWith('-'));

const r = spawnSync('wsl', [...distro, '--', 'bash', toWsl(join(root, 'scripts', 'linux-build.sh')), toWsl(root), ...targets], { stdio: 'inherit' });
if (r.error) { console.error('[dist:linux] WSL is not available. Install it with: wsl --install -d Ubuntu-24.04'); process.exit(1); }
process.exit(r.status ?? 1);

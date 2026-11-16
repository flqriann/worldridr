// Runs the built forge-server. Extra arguments are passed through, e.g.
//   npm run server -- --port 8787 --data ./server-data --publish-key secret
import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const bin = join(root, 'server', 'target', 'release', process.platform === 'win32' ? 'forge-server.exe' : 'forge-server');
if (!existsSync(bin)) { console.error('[server] forge-server is not built yet – run: npm run server:build'); process.exit(1); }
const child = spawn(bin, process.argv.slice(2), { stdio: 'inherit', cwd: root });
child.on('exit', (code) => process.exit(code ?? 0));
for (const sig of ['SIGINT', 'SIGTERM']) process.on(sig, () => child.kill(sig));

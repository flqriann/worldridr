// Builds the distributable programs into release/:
//   release/studio/  Forge Studio   – installer + portable .exe (Windows), AppImage + tar.gz (Linux)
//   release/player/  Forge Player   – same formats
//   release/server/  Forge Server   – standalone binary + README (forge-server.exe / forge-server)
//
// Usage: node scripts/dist.mjs [studio] [player] [server]   (default: all three)
//        Builds for the current OS. Linux packages from Windows: npm run dist:linux (WSL).
import { spawnSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync, chmodSync } from 'node:fs';
import { homedir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const version = JSON.parse(readFileSync(join(root, 'package.json'), 'utf8')).version;
const win = process.platform === 'win32';
const os = win ? 'win' : process.platform === 'darwin' ? 'mac' : 'linux';
const asked = process.argv.slice(2).filter((a) => !a.startsWith('-'));
const want = new Set(asked.length ? asked : ['studio', 'player', 'server']);
for (const w of want) if (!['studio', 'player', 'server'].includes(w)) { console.error(`unknown target "${w}" (studio, player, server)`); process.exit(1); }

const run = (cmd, args, opts = {}) => {
  console.log(`\n> ${cmd} ${args.join(' ')}`);
  const r = spawnSync(cmd, args, { cwd: root, stdio: 'inherit', shell: win, ...opts });
  if (r.status !== 0) { console.error(`\n[dist] failed: ${cmd} ${args.join(' ')}`); process.exit(r.status ?? 1); }
};
const exe = win ? 'cargo.exe' : 'cargo';
const cargo = [join(homedir(), '.cargo', 'bin', exe), 'cargo'].find((c) => c === 'cargo' ? spawnSync('cargo', ['--version']).status === 0 : existsSync(c));

// 1. Rust server (needed for the server package and bundled into Studio)
if (want.has('server') || want.has('studio')) {
  if (!cargo) { console.error('[dist] Rust (cargo) not found – install it from https://rustup.rs'); process.exit(1); }
  run(cargo, ['build', '--release'], { cwd: join(root, 'server'), shell: false });
}

// 2. Web build (shared by Studio and Player) + icons
if (want.has('studio') || want.has('player')) {
  run('npm', ['run', 'build']);
  run(process.execPath, ['scripts/make-icons.mjs'], { shell: false });
}

// electron-builder's winCodeSign archive contains macOS symlinks that Windows can only
// create with admin rights / developer mode. Pre-extract it without them.
async function prepareWinCodeSign() {
  const cache = join(process.env.LOCALAPPDATA ?? join(homedir(), 'AppData', 'Local'), 'electron-builder', 'Cache', 'winCodeSign');
  const target = join(cache, 'winCodeSign-2.6.0');
  if (existsSync(join(target, 'windows-10'))) return;
  console.log('[dist] preparing winCodeSign-2.6.0 (without macOS symlinks)');
  mkdirSync(cache, { recursive: true });
  const archive = join(cache, 'winCodeSign-2.6.0.7z');
  if (!existsSync(archive)) {
    const res = await fetch('https://github.com/electron-userland/electron-builder-binaries/releases/download/winCodeSign-2.6.0/winCodeSign-2.6.0.7z');
    if (!res.ok) throw new Error(`download failed: ${res.status}`);
    writeFileSync(archive, Buffer.from(await res.arrayBuffer()));
  }
  const sevenZip = join(root, 'node_modules', '7zip-bin', 'win', 'x64', '7za.exe');
  const r = spawnSync(sevenZip, ['x', '-bd', '-y', '-xr!darwin', archive, `-o${target}`], { stdio: 'inherit' });
  if (r.status !== 0) { console.error('[dist] could not extract winCodeSign'); process.exit(1); }
}
if (win && (want.has('studio') || want.has('player'))) await prepareWinCodeSign();

// 3. Desktop apps
const builder = join(root, 'node_modules', '.bin', win ? 'electron-builder.cmd' : 'electron-builder');
for (const app of ['studio', 'player']) {
  if (!want.has(app)) continue;
  rmSync(join(root, 'release', app), { recursive: true, force: true });
  run(builder, ['--config', `electron-builder.${app}.cjs`, `--${os}`, '--publish', 'never']);
}

// 4. Server package
if (want.has('server')) {
  const bin = win ? 'forge-server.exe' : 'forge-server';
  const dir = join(root, 'release', 'server', `ForgeServer-${version}-${os}-x64`);
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  copyFileSync(join(root, 'server', 'target', 'release', bin), join(dir, bin));
  if (!win) chmodSync(join(dir, bin), 0o755);
  writeFileSync(join(dir, 'README.txt'), [
    `Forge Server ${version}`,
    '',
    `Start:  ${win ? 'forge-server.exe' : './forge-server'} [--port 8787] [--host 0.0.0.0] [--data ./server-data] [--publish-key KEY]`,
    '',
    'The publish key is printed at startup and stored in server-data/server.json.',
    'Forge Studio needs it to publish (File > Publish to Server). Players connect',
    'with Forge Player to http://<this-computer>:8787 and need no key.',
    'Open TCP port 8787 in the firewall for other computers. For the internet, put',
    'an HTTPS reverse proxy (e.g. Caddy, nginx) in front of the server.',
    '',
  ].join(win ? '\r\n' : '\n'));
  if (win) writeFileSync(join(dir, 'start-server.bat'), '@echo off\r\ncd /d "%~dp0"\r\nforge-server.exe --data server-data\r\npause\r\n');
  else { writeFileSync(join(dir, 'start-server.sh'), '#!/bin/sh\ncd "$(dirname "$0")"\nexec ./forge-server --data server-data "$@"\n'); chmodSync(join(dir, 'start-server.sh'), 0o755); }
  if (!win) run('tar', ['-czf', `ForgeServer-${version}-${os}-x64.tar.gz`, `ForgeServer-${version}-${os}-x64`], { cwd: join(root, 'release', 'server'), shell: false });
  else run('powershell', ['-NoProfile', '-Command', `Compress-Archive -Force -Path 'ForgeServer-${version}-${os}-x64' -DestinationPath 'ForgeServer-${version}-${os}-x64.zip'`], { cwd: join(root, 'release', 'server'), shell: false });
  console.log(`[dist] server: ${dir}`);
}

console.log('\n[dist] done – see release/');

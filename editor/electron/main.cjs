// Forge – Electron main process for both desktop apps:
//   * Forge Studio (default)        – the editor
//   * Forge Player (--player)       – browse and join published games
// Responsibilities: splash + main window, serving the renderer through the
// privileged app:// protocol (WASM + workers), a narrow file-system IPC API
// (see preload.cjs), the local project library, language-pack folder and
// starting/stopping a local Forge Server (Rust) for testing.
const { app, BrowserWindow, ipcMain, dialog, protocol, shell, Menu } = require('electron');
const path = require('node:path');
const fs = require('node:fs/promises');
const fsSync = require('node:fs');
const { spawn } = require('node:child_process');
const { runSmokeTest, runBenchmark } = require('./diagnostics.cjs');

const ROOT = path.join(__dirname, '..');
const DIST = path.join(ROOT, 'dist');
const DEV_URL = process.env.FORGE_DEV_URL;
const PKG = require(path.join(ROOT, 'package.json'));
// Packaged builds bake the mode into package.json ("forgeMode", see electron-builder.*.cjs);
// in development it comes from --player / FORGE_MODE.
const MODE = PKG.forgeMode === 'player' || process.argv.includes('--player') || process.env.FORGE_MODE === 'player' ? 'player' : 'studio';
const VERSION = PKG.version;
// Studio and Player keep separate browser profiles (two processes cannot share one),
// but share the language-pack folder.
app.setName(MODE === 'player' ? 'Forge Player' : 'WorldRidr Studio');
app.setPath('userData', path.join(app.getPath('appData'), MODE === 'player' ? 'Forge Player' : 'WorldRidr Studio'));

protocol.registerSchemesAsPrivileged([
  { scheme: 'app', privileges: { standard: true, secure: true, supportFetchAPI: true, corsEnabled: true, stream: true } },
]);

const MIME = {
  '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css',
  '.wasm': 'application/wasm', '.json': 'application/json', '.png': 'image/png', '.svg': 'image/svg+xml',
  '.ttf': 'font/ttf', '.woff': 'font/woff', '.woff2': 'font/woff2', '.jpg': 'image/jpeg',
};

// WASM needs 'wasm-unsafe-eval'; media are data:/blob: URLs; connect-src allows game servers.
const CSP = [
  "default-src 'self'",
  "script-src 'self' 'wasm-unsafe-eval'",
  "style-src 'self' 'unsafe-inline'",
  "img-src 'self' data: blob: http: https:",
  "media-src 'self' data: blob:",
  "font-src 'self' data:",
  "worker-src 'self' blob:",
  "connect-src 'self' data: blob: http: https: ws: wss:",
].join('; ');

const autosaveDir = () => path.join(app.getPath('userData'), 'autosave');
const languagesDir = () => path.join(app.getPath('appData'), 'Forge', 'languages');
const libraryDir = () => path.join(app.getPath('documents'), 'WorldRidr Studio', 'Projects');
const safeId = (id) => String(id).replace(/[^a-z0-9_-]/gi, '');

const FILTERS = {
  project: [{ name: 'Forge Project', extensions: ['forge'] }, { name: 'Forge Scene', extensions: ['forgescene'] }, { name: 'JSON', extensions: ['json'] }],
  scene: [{ name: 'Forge Scene', extensions: ['forgescene'] }],
  model: [{ name: 'Forge Model', extensions: ['forgemodel'] }],
  glb: [{ name: 'glTF Binary', extensions: ['glb'] }],
  lua: [{ name: 'Lua Script', extensions: ['lua', 'luau', 'txt'] }],
  language: [{ name: 'Forge Language Pack', extensions: ['json'] }],
  image: [{ name: 'Images', extensions: ['png', 'jpg', 'jpeg', 'webp'] }],
  sound: [{ name: 'Audio', extensions: ['mp3', 'wav', 'ogg'] }],
  asset: [{ name: 'Assets', extensions: ['glb', 'gltf', 'png', 'jpg', 'jpeg', 'webp', 'mp3', 'wav', 'ogg', 'lua', 'json', 'forgemodel'] }],
};

// ---- windows ----------------------------------------------------------------------

let splash = null;
let mainWin = null;

function createSplash() {
  splash = new BrowserWindow({
    width: 560, height: 320, frame: false, transparent: true, resizable: false, movable: true, show: false,
    alwaysOnTop: false, skipTaskbar: false, backgroundColor: '#00000000', title: MODE === 'player' ? 'Forge Player' : 'Forge Studio',
    webPreferences: { contextIsolation: true, sandbox: true },
  });
  const q = new URLSearchParams({ product: MODE === 'player' ? 'Player' : 'Studio', version: VERSION, tagline: MODE === 'player' ? 'Join games made with Forge Studio' : 'Build, script and play 3D games' });
  splash.loadFile(path.join(__dirname, 'splash.html'), { search: q.toString() });
  splash.once('ready-to-show', () => splash && splash.show());
}

function setSplashStatus(text) {
  if (splash && !splash.isDestroyed()) splash.webContents.executeJavaScript(`window.setStatus && window.setStatus(${JSON.stringify(text)})`).catch(() => {});
}

function showMain() {
  if (!mainWin || mainWin.isDestroyed()) return;
  if (!mainWin.isVisible()) { mainWin.show(); mainWin.focus(); }
  if (splash && !splash.isDestroyed()) { splash.destroy(); splash = null; }
}

function createWindow() {
  const win = new BrowserWindow({
    width: 1600, height: 960, minWidth: 960, minHeight: 620,
    backgroundColor: '#15161a', title: MODE === 'player' ? 'Forge Player' : 'Forge Studio', show: false,
    webPreferences: { preload: path.join(__dirname, 'preload.cjs'), contextIsolation: true, sandbox: false, additionalArguments: [`--forge-mode=${MODE}`], backgroundThrottling: false },
  });
  mainWin = win;
  Menu.setApplicationMenu(null); // the renderer draws its own menu bar
  win.webContents.setWindowOpenHandler(({ url }) => { shell.openExternal(url); return { action: 'deny' }; });
  win.webContents.on('did-start-loading', () => setSplashStatus('Loading interface…'));
  // fallback: never keep the splash forever
  win.once('ready-to-show', () => setTimeout(showMain, 15000));
  let allowClose = false;
  win.on('close', (e) => {
    if (allowClose || MODE === 'player') return;
    e.preventDefault();
    win.webContents.send('app:close-requested');
  });
  ipcMain.removeHandler('app:confirm-close');
  ipcMain.handle('app:confirm-close', () => { allowClose = true; win.close(); });
  const page = MODE === 'player' ? 'player.html' : 'index.html';
  if (DEV_URL) win.loadURL(`${DEV_URL}/${MODE === 'player' ? 'player.html' : ''}`);
  else win.loadURL(`app://forge/${page}`);
  if (process.argv.includes('--devtools')) win.webContents.openDevTools();
  if (process.argv.includes('--smoke-test')) runSmokeTest(win);
  if (process.argv.includes('--bench')) runBenchmark(win);
  const headless = process.argv.includes('--smoke-test') || process.argv.includes('--bench');
  if (headless) win.once('ready-to-show', showMain);
}

// ---- local Forge Server (Rust) ---------------------------------------------------------

let serverProc = null;
let serverInfo = { running: false, url: null, error: null, log: [] };

function serverBinary() {
  const exe = process.platform === 'win32' ? 'forge-server.exe' : 'forge-server';
  const candidates = [path.join(ROOT, 'server', 'target', 'release', exe), path.join(process.resourcesPath || ROOT, exe)];
  return candidates.find((c) => fsSync.existsSync(c)) || null;
}

function startServer(port = 8787) {
  if (serverProc) return serverInfo;
  const bin = serverBinary();
  if (!bin) { serverInfo = { running: false, url: null, error: 'Forge Server is not built yet. Run "npm run server:build".', log: [] }; return serverInfo; }
  const dataDir = path.join(app.getPath('userData'), 'server-data');
  // a stable publish key for the local server, kept next to its data
  const keyFile = path.join(app.getPath('userData'), 'local-server.json');
  let publishKey;
  try { publishKey = JSON.parse(fsSync.readFileSync(keyFile, 'utf8')).publishKey; } catch { /* first start */ }
  if (!publishKey) {
    publishKey = require('node:crypto').randomBytes(16).toString('hex');
    fsSync.mkdirSync(path.dirname(keyFile), { recursive: true });
    fsSync.writeFileSync(keyFile, JSON.stringify({ publishKey }));
  }
  serverProc = spawn(bin, ['--port', String(port), '--data', dataDir, '--publish-key', publishKey], { stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
  serverInfo = { running: true, url: `http://127.0.0.1:${port}`, publishKey, error: null, log: [] };
  const push = (d) => { serverInfo.log.push(...String(d).split(/\r?\n/).filter(Boolean)); serverInfo.log = serverInfo.log.slice(-200); };
  serverProc.stdout.on('data', push);
  serverProc.stderr.on('data', push);
  serverProc.on('exit', (code) => {
    serverInfo = { ...serverInfo, running: false, error: code ? `Server exited with code ${code}` : null };
    serverProc = null;
  });
  return serverInfo;
}

function stopServer() {
  if (serverProc) { serverProc.kill(); serverProc = null; }
  serverInfo = { ...serverInfo, running: false };
  return serverInfo;
}

// ---- app ------------------------------------------------------------------------------

app.whenReady().then(() => {
  protocol.handle('app', async (request) => {
    const url = new URL(request.url);
    let rel = decodeURIComponent(url.pathname);
    if (rel === '/' || rel === '') rel = '/index.html';
    const file = path.normalize(path.join(DIST, rel));
    if (!file.startsWith(DIST)) return new Response('Forbidden', { status: 403 });
    try {
      const data = await fs.readFile(file);
      const headers = { 'content-type': MIME[path.extname(file)] || 'application/octet-stream' };
      if (path.extname(file) === '.html') headers['content-security-policy'] = CSP;
      return new Response(data, { headers });
    } catch {
      return new Response('Not found', { status: 404 });
    }
  });

  // ---- world import: HTTP GET/POST for the map services (allow-listed hosts only) ----
  const NET_HOSTS = new Set(['nominatim.openstreetmap.org', 'overpass-api.de', 'overpass.kumi.systems', 'overpass.private.coffee', 's3.amazonaws.com', 'elevation-tiles-prod.s3.amazonaws.com']);
  ipcMain.handle('net:request', async (_e, { url, method, body, binary }) => {
    const u = new URL(url);
    if (u.protocol !== 'https:' || !NET_HOSTS.has(u.hostname)) throw new Error('Host not allowed: ' + u.hostname);
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), 70000);
    try {
      const res = await fetch(url, {
        method: method || 'GET', body: body || undefined, signal: ctl.signal,
        headers: { 'User-Agent': 'WorldRidrStudio/0.1 (worldridr map editor)', ...(body ? { 'Content-Type': 'application/x-www-form-urlencoded' } : {}) },
      });
      if (!res.ok) throw new Error(`HTTP ${res.status} from ${u.hostname}`);
      if (binary) { const buf = Buffer.from(await res.arrayBuffer()); return buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength); }
      return await res.text();
    } finally { clearTimeout(timer); }
  });
  // folder dialogs / folder writes for the worldridr-map format
  ipcMain.handle('map:pickFolder', async (e, { title, create }) => {
    const r = await dialog.showOpenDialog(BrowserWindow.fromWebContents(e.sender), { title, properties: ['openDirectory', ...(create ? ['createDirectory'] : [])] });
    return r.canceled ? null : r.filePaths[0];
  });
  ipcMain.handle('map:readDir', async (_e, dir) => {
    const out = {};
    const walk = async (rel) => {
      for (const ent of await fs.readdir(path.join(dir, rel), { withFileTypes: true })) {
        const r = rel ? rel + '/' + ent.name : ent.name;
        if (ent.isDirectory()) { if (ent.name !== '.git' && ent.name !== 'node_modules') await walk(r); }
        else if (/\.(json|lua|txt)$/i.test(ent.name)) out[r] = await fs.readFile(path.join(dir, r), 'utf8');
      }
    };
    await walk('');
    return out;
  });
  ipcMain.handle('map:writeFiles', async (_e, { dir, files, removeStale }) => {
    // files: { 'tiles/z15_x1_y2.json': 'text', ... }; unchanged files are left untouched (git-friendly)
    let written = 0;
    for (const [rel, text] of Object.entries(files)) {
      const p = path.resolve(dir, rel);
      if (!p.startsWith(path.resolve(dir) + path.sep)) throw new Error('Bad path: ' + rel);
      await fs.mkdir(path.dirname(p), { recursive: true });
      let old = null;
      try { old = await fs.readFile(p, 'utf8'); } catch { /* new file */ }
      if (old === text) continue;
      await fs.writeFile(p + '.tmp', text);
      await fs.rename(p + '.tmp', p);
      written++;
    }
    let removed = 0;
    for (const sub of removeStale || []) { // directories whose unlisted *.json files are deleted (tiles/)
      const d = path.join(dir, sub);
      let list = [];
      try { list = await fs.readdir(d); } catch { continue; }
      for (const f of list) if (f.endsWith('.json') && !(sub + '/' + f in files)) { await fs.unlink(path.join(d, f)); removed++; }
    }
    return { written, removed };
  });

  // git for map repositories (only a fixed set of sub-commands, no hooks/config overrides)
  const GIT_OK = new Set(['init', 'clone', 'status', 'add', 'commit', 'pull', 'push', 'fetch', 'remote', 'log', 'branch', 'checkout', 'rev-parse', 'config', 'diff', 'merge']);
  ipcMain.handle('git:run', (_e, { dir, args }) => new Promise((resolve) => {
    if (!Array.isArray(args) || !GIT_OK.has(args[0])) return resolve({ code: 1, out: '', err: 'git command not allowed' });
    if (args.some((a) => typeof a !== 'string' || /^(-c|--config|--exec|--upload-pack|--receive-pack|-u?--no-verify)/.test(a) && a !== '-u')) return resolve({ code: 1, out: '', err: 'git option not allowed' });
    require('node:child_process').execFile('git', args, { cwd: dir, timeout: 180000, maxBuffer: 16 * 1024 * 1024, env: { ...process.env, GIT_TERMINAL_PROMPT: '0', LC_ALL: 'C' } },
      (err, out, errOut) => resolve({ code: err ? (typeof err.code === 'number' ? err.code : 1) : 0, out: String(out || ''), err: String(errOut || (err && err.message) || '') }));
  }));

  ipcMain.handle('fs:openDialog', async (e, { kind, multi }) => {
    const r = await dialog.showOpenDialog(BrowserWindow.fromWebContents(e.sender), {
      properties: multi ? ['openFile', 'multiSelections'] : ['openFile'],
      filters: FILTERS[kind] || [{ name: 'All Files', extensions: ['*'] }],
    });
    return r.canceled ? [] : r.filePaths;
  });
  ipcMain.handle('fs:saveDialog', async (e, { kind, defaultName }) => {
    const r = await dialog.showSaveDialog(BrowserWindow.fromWebContents(e.sender), {
      defaultPath: defaultName, filters: FILTERS[kind] || [{ name: 'All Files', extensions: ['*'] }],
    });
    return r.canceled ? null : r.filePath;
  });
  ipcMain.handle('fs:readFile', async (_e, p) => {
    const buf = await fs.readFile(p);
    return buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
  });
  ipcMain.handle('fs:writeFile', async (_e, p, data) => {
    // Atomic write: temp file + rename, so a crash never leaves a half-written project.
    const tmp = p + '.tmp';
    await fs.writeFile(tmp, typeof data === 'string' ? data : Buffer.from(data));
    await fs.rename(tmp, p);
    return true;
  });

  // autosave
  ipcMain.handle('autosave:write', async (_e, id, text) => {
    await fs.mkdir(autosaveDir(), { recursive: true });
    await fs.writeFile(path.join(autosaveDir(), safeId(id) + '.forge'), text);
    return true;
  });
  ipcMain.handle('autosave:list', async () => {
    try {
      const names = await fs.readdir(autosaveDir());
      const out = [];
      for (const n of names.filter((x) => x.endsWith('.forge'))) {
        const full = path.join(autosaveDir(), n);
        const st = await fs.stat(full);
        out.push({ id: n.replace(/\.forge$/, ''), modified: st.mtimeMs, text: await fs.readFile(full, 'utf8') });
      }
      return out;
    } catch { return []; }
  });
  ipcMain.handle('autosave:remove', async (_e, id) => {
    try { await fs.unlink(path.join(autosaveDir(), safeId(id) + '.forge')); } catch { /* already gone */ }
    return true;
  });

  // project library: <Documents>/Forge Studio/Projects/<name>.forge (+ .meta.json sidecar)
  ipcMain.handle('library:dir', async () => { await fs.mkdir(libraryDir(), { recursive: true }); return libraryDir(); });
  ipcMain.handle('library:list', async () => {
    await fs.mkdir(libraryDir(), { recursive: true });
    const out = [];
    for (const n of await fs.readdir(libraryDir())) {
      if (!n.endsWith('.forge')) continue;
      const full = path.join(libraryDir(), n);
      const st = await fs.stat(full);
      let meta = {};
      try { meta = JSON.parse(await fs.readFile(full + '.meta.json', 'utf8')); } catch { /* no sidecar */ }
      out.push({ path: full, name: meta.name || n.replace(/\.forge$/, ''), modified: st.mtimeMs, size: st.size, thumbnail: meta.thumbnail || null });
    }
    return out;
  });
  ipcMain.handle('library:writeMeta', async (_e, p, meta) => {
    await fs.writeFile(p + '.meta.json', JSON.stringify(meta));
    return true;
  });
  ipcMain.handle('library:trash', async (_e, p) => {
    await shell.trashItem(p); // recoverable from the recycle bin
    try { await shell.trashItem(p + '.meta.json'); } catch { /* no sidecar */ }
    return true;
  });
  ipcMain.handle('library:reveal', (_e, p) => { shell.showItemInFolder(p); return true; });
  ipcMain.handle('library:uniquePath', async (_e, name) => {
    await fs.mkdir(libraryDir(), { recursive: true });
    const base = String(name).replace(/[\\/:*?"<>|]/g, '_').trim() || 'Project';
    let p = path.join(libraryDir(), base + '.forge');
    for (let i = 2; fsSync.existsSync(p); i++) p = path.join(libraryDir(), `${base} (${i}).forge`);
    return p;
  });

  // language packs dropped into <userData>/languages are loaded at startup
  ipcMain.handle('lang:list', async () => {
    await fs.mkdir(languagesDir(), { recursive: true });
    const out = [];
    for (const n of await fs.readdir(languagesDir())) if (n.endsWith('.json')) {
      try { out.push(await fs.readFile(path.join(languagesDir(), n), 'utf8')); } catch { /* unreadable */ }
    }
    return out;
  });
  ipcMain.handle('lang:openFolder', async () => { await fs.mkdir(languagesDir(), { recursive: true }); shell.openPath(languagesDir()); return languagesDir(); });

  // local server
  ipcMain.handle('server:start', (_e, port) => startServer(port));
  ipcMain.handle('server:stop', () => stopServer());
  ipcMain.handle('server:status', () => serverInfo);
  ipcMain.handle('app:launchPlayer', () => {
    // packaged: the Studio executable itself can run in player mode
    const args = app.isPackaged ? ['--player'] : [ROOT, '--player'];
    spawn(process.execPath, args, { detached: true, stdio: 'ignore', env: { ...process.env, FORGE_MODE: 'player' } }).unref();
    return true;
  });

  ipcMain.handle('app:setTitle', (e, title) => { BrowserWindow.fromWebContents(e.sender)?.setTitle(title); });
  ipcMain.handle('app:ready', () => { showMain(); return true; });
  ipcMain.handle('app:status', (_e, text) => { setSplashStatus(text); return true; });
  ipcMain.handle('app:mode', () => MODE);

  const headless = process.argv.includes('--smoke-test') || process.argv.includes('--bench');
  if (!headless) createSplash();
  createWindow();
  app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow(); });
});

app.on('window-all-closed', () => { stopServer(); if (process.platform !== 'darwin') app.quit(); });
app.on('before-quit', () => stopServer());

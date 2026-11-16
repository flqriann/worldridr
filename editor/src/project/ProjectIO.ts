// File system abstraction: native dialogs + files in Electron, File System
// Access API (or download fallback) in a plain browser. Also autosave storage.

export type FileKind = 'project' | 'scene' | 'model' | 'glb' | 'lua' | 'asset' | 'language' | 'image' | 'sound';

interface NativeApi {
  isElectron: true;
  openDialog(kind: FileKind, multi?: boolean): Promise<string[]>;
  saveDialog(kind: FileKind, defaultName: string): Promise<string | null>;
  readFile(p: string): Promise<ArrayBuffer>;
  writeFile(p: string, data: string | ArrayBuffer): Promise<boolean>;
  autosaveWrite(id: string, text: string): Promise<boolean>;
  autosaveList(): Promise<{ id: string; modified: number; text: string }[]>;
  autosaveRemove(id: string): Promise<boolean>;
  setTitle(t: string): Promise<void>;
  confirmClose(): Promise<void>;
  onCloseRequested(cb: () => void): void;
  mode: 'studio' | 'player';
  libraryDir(): Promise<string>;
  libraryList(): Promise<{ path: string; name: string; modified: number; size: number; thumbnail: string | null }[]>;
  libraryWriteMeta(p: string, meta: { name: string; thumbnail: string | null }): Promise<boolean>;
  libraryTrash(p: string): Promise<boolean>;
  libraryReveal(p: string): Promise<boolean>;
  libraryUniquePath(name: string): Promise<string>;
  languageList(): Promise<string[]>;
  languageOpenFolder(): Promise<string>;
  serverStart(port?: number): Promise<ServerInfo>;
  serverStop(): Promise<ServerInfo>;
  serverStatus(): Promise<ServerInfo>;
  launchPlayer(): Promise<boolean>;
  ready(): Promise<boolean>;
  status(text: string): Promise<boolean>;
  gitRun(o: { dir: string; args: string[] }): Promise<{ code: number; out: string; err: string }>;
  netRequest(o: { url: string; method?: string; body?: string; binary?: boolean }): Promise<string | ArrayBuffer>;
  pickFolder(o: { title: string; create?: boolean }): Promise<string | null>;
  readMapDir(dir: string): Promise<Record<string, string>>;
  writeMapFiles(o: { dir: string; files: Record<string, string>; removeStale?: string[] }): Promise<{ written: number; removed: number }>;
}

export interface ServerInfo { running: boolean; url: string | null; publishKey?: string; error: string | null; log: string[] }

declare global { interface Window { forgeNative?: NativeApi } }

export const native: NativeApi | undefined = window.forgeNative;
export const isElectron = !!native;

const ACCEPT: Record<FileKind, string[]> = {
  project: ['.forge', '.forgescene', '.json'],
  scene: ['.forgescene'],
  model: ['.forgemodel'],
  glb: ['.glb'],
  lua: ['.lua', '.luau', '.txt'],
  asset: ['.glb', '.gltf', '.png', '.jpg', '.jpeg', '.webp', '.mp3', '.wav', '.ogg', '.lua', '.json', '.forgemodel'],
  language: ['.json'],
  image: ['.png', '.jpg', '.jpeg', '.webp'],
  sound: ['.mp3', '.wav', '.ogg'],
};
const DESCRIPTION: Record<FileKind, string> = {
  project: 'Forge Project', scene: 'Forge Scene', model: 'Forge Model', glb: 'glTF Binary', lua: 'Lua Script', asset: 'Asset', language: 'Language Pack', image: 'Image', sound: 'Audio',
};

export interface OpenedFile { name: string; path: string | null; handle?: FileSystemFileHandle; data: ArrayBuffer }
export interface SavedFile { name: string; path: string | null; handle?: FileSystemFileHandle }

const baseName = (p: string) => p.replace(/^.*[\\/]/, '');

export async function openFiles(kind: FileKind, multi = false): Promise<OpenedFile[]> {
  if (native) {
    const paths = await native.openDialog(kind, multi);
    const out: OpenedFile[] = [];
    for (const p of paths) out.push({ name: baseName(p), path: p, data: await native.readFile(p) });
    return out;
  }
  const w = window as any;
  if (typeof w.showOpenFilePicker === 'function') {
    try {
      const handles: FileSystemFileHandle[] = await w.showOpenFilePicker({
        multiple: multi, types: [{ description: DESCRIPTION[kind], accept: { 'application/octet-stream': ACCEPT[kind] } }],
      });
      const out: OpenedFile[] = [];
      for (const h of handles) { const f = await h.getFile(); out.push({ name: f.name, path: null, handle: h, data: await f.arrayBuffer() }); }
      return out;
    } catch (e) {
      if ((e as Error).name === 'AbortError') return [];
      throw e;
    }
  }
  return new Promise((resolve) => {
    const input = document.createElement('input');
    input.type = 'file';
    input.multiple = multi;
    input.accept = ACCEPT[kind].join(',');
    input.onchange = async () => {
      const files = [...(input.files ?? [])];
      resolve(await Promise.all(files.map(async (f) => ({ name: f.name, path: null, data: await f.arrayBuffer() }))));
    };
    input.click();
  });
}

export async function readPath(path: string): Promise<ArrayBuffer> {
  if (!native) throw new Error('Opening files by path requires the desktop app.');
  return native.readFile(path);
}

/**
 * Saves data. If `target` (path or handle) is given it is overwritten,
 * otherwise a save dialog is shown. Returns null if the user cancelled.
 */
export async function saveFile(kind: FileKind, defaultName: string, data: string | ArrayBuffer, target?: { path?: string | null; handle?: FileSystemFileHandle }): Promise<SavedFile | null> {
  if (native) {
    let p = target?.path ?? null;
    if (!p) p = await native.saveDialog(kind, defaultName);
    if (!p) return null;
    await native.writeFile(p, data);
    return { name: baseName(p), path: p };
  }
  const w = window as any;
  let handle = target?.handle;
  if (!handle && typeof w.showSaveFilePicker === 'function') {
    try {
      handle = await w.showSaveFilePicker({ suggestedName: defaultName, types: [{ description: DESCRIPTION[kind], accept: { 'application/octet-stream': [ACCEPT[kind][0]] } }] });
    } catch (e) {
      if ((e as Error).name === 'AbortError') return null;
      throw e;
    }
  }
  if (handle) {
    const ws = await (handle as any).createWritable();
    await ws.write(data);
    await ws.close();
    return { name: handle.name, path: null, handle };
  }
  // Fallback: browser download (the user picks the location in the browser).
  const blob = new Blob([data], { type: 'application/octet-stream' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = defaultName;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  return { name: defaultName, path: null };
}

// ---- Autosave ------------------------------------------------------------------

const DB = 'forge-studio';
const STORE = 'autosave';

function idb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB, 1);
    req.onupgradeneeded = () => req.result.createObjectStore(STORE);
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

async function idbOp<T>(mode: IDBTransactionMode, fn: (s: IDBObjectStore) => IDBRequest<T>): Promise<T> {
  const db = await idb();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, mode);
    const r = fn(tx.objectStore(STORE));
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
}

export interface AutosaveEntry { id: string; modified: number; text: string }

export const autosave = {
  async write(id: string, text: string) {
    if (native) await native.autosaveWrite(id, text);
    else await idbOp('readwrite', (s) => s.put({ id, modified: Date.now(), text }, id));
  },
  async list(): Promise<AutosaveEntry[]> {
    if (native) return native.autosaveList();
    try {
      const all = await idbOp<AutosaveEntry[]>('readonly', (s) => s.getAll() as IDBRequest<AutosaveEntry[]>);
      return all ?? [];
    } catch { return []; }
  },
  async remove(id: string) {
    if (native) await native.autosaveRemove(id);
    else await idbOp('readwrite', (s) => s.delete(id));
  },
};

// ---- Recent files (desktop only) ------------------------------------------------------

export interface RecentFile { path: string; name: string; time: number }
const RECENT_KEY = 'forge.recent';

export function getRecent(): RecentFile[] {
  try { return JSON.parse(localStorage.getItem(RECENT_KEY) ?? '[]'); } catch { return []; }
}
export function addRecent(path: string, name: string) {
  const list = getRecent().filter((r) => r.path !== path);
  list.unshift({ path, name, time: Date.now() });
  try { localStorage.setItem(RECENT_KEY, JSON.stringify(list.slice(0, 12))); } catch { /* storage full/blocked */ }
}
export function removeRecent(path: string) {
  try { localStorage.setItem(RECENT_KEY, JSON.stringify(getRecent().filter((r) => r.path !== path))); } catch { /* ignore */ }
}

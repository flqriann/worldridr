// "My Projects": the local project library shown on the start page.
// Desktop: files in <Documents>/Forge Studio/Projects (+ .meta.json with name
// and thumbnail). Browser: IndexedDB. Saving a project that has no file yet
// puts it here – no dialog needed, like a cloud save in Roblox Studio.

import { native } from './ProjectIO';

export interface LibraryEntry {
  /** Desktop: absolute file path. Browser: IndexedDB key. */
  key: string;
  name: string;
  modified: number;
  size: number;
  thumbnail: string | null;
  location: 'folder' | 'browser';
}

const DB = 'forge-library';
const STORE = 'projects';

function idb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB, 1);
    req.onupgradeneeded = () => req.result.createObjectStore(STORE);
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}
async function tx<T>(mode: IDBTransactionMode, fn: (s: IDBObjectStore) => IDBRequest<T>): Promise<T> {
  const db = await idb();
  return new Promise((resolve, reject) => {
    const r = fn(db.transaction(STORE, mode).objectStore(STORE));
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
}

interface StoredProject { key: string; name: string; modified: number; thumbnail: string | null; text: string }

export const library = {
  async list(): Promise<LibraryEntry[]> {
    if (native) {
      const list = await native.libraryList();
      return list.map((e) => ({ key: e.path, name: e.name, modified: e.modified, size: e.size, thumbnail: e.thumbnail, location: 'folder' as const }))
        .sort((a, b) => b.modified - a.modified);
    }
    try {
      const all = (await tx<StoredProject[]>('readonly', (s) => s.getAll() as IDBRequest<StoredProject[]>)) ?? [];
      return all.map((p) => ({ key: p.key, name: p.name, modified: p.modified, size: p.text.length, thumbnail: p.thumbnail, location: 'browser' as const }))
        .sort((a, b) => b.modified - a.modified);
    } catch { return []; }
  },

  /** Reserves a key/path for a new project with this name. */
  async newKey(name: string): Promise<string> {
    if (native) return native.libraryUniquePath(name);
    return 'p-' + Date.now().toString(36) + Math.random().toString(36).slice(2, 6);
  },

  async save(key: string, name: string, text: string, thumbnail: string | null) {
    if (native) {
      await native.writeFile(key, text);
      await native.libraryWriteMeta(key, { name, thumbnail });
      return;
    }
    await tx('readwrite', (s) => s.put({ key, name, modified: Date.now(), thumbnail, text } satisfies StoredProject, key));
  },

  async read(key: string): Promise<string> {
    if (native) return new TextDecoder().decode(await native.readFile(key));
    const p = await tx<StoredProject | undefined>('readonly', (s) => s.get(key) as IDBRequest<StoredProject | undefined>);
    if (!p) throw new Error('This project no longer exists in the browser library.');
    return p.text;
  },

  /** Desktop: moves the file to the recycle bin. Browser: deletes permanently. */
  async remove(key: string) {
    if (native) await native.libraryTrash(key);
    else await tx('readwrite', (s) => s.delete(key));
  },

  async rename(key: string, name: string) {
    if (native) {
      const list = await native.libraryList();
      const e = list.find((x) => x.path === key);
      await native.libraryWriteMeta(key, { name, thumbnail: e?.thumbnail ?? null });
      return;
    }
    const p = await tx<StoredProject | undefined>('readonly', (s) => s.get(key) as IDBRequest<StoredProject | undefined>);
    if (p) await tx('readwrite', (s) => s.put({ ...p, name }, key));
  },

  reveal(key: string) { native?.libraryReveal(key); },

  isLibraryKey(key: string | null | undefined): boolean {
    if (!key) return false;
    return native ? /[\\/]WorldRidr Studio[\\/]Projects[\\/]/.test(key) : key.startsWith('p-');
  },
};

// Studio actions for the world features: place import, worldridr-map save/open, git collaboration.

import type { Studio } from '../editor/Studio';
import type { Document } from '../editor/Document';
import { native } from '../project/ProjectIO';
import { importPlace, type ImportOptions } from './importer';
import { exportMap, importMap, type ExportOptions } from './mapformat';

export const mapOptions: ExportOptions = { includeOsm: false, terrain: false, scripts: true };

const need = (studio: Studio) => {
  if (!native) { studio.output.error('Map folders and git need the desktop app (npm run app).'); return false; }
  return true;
};

export async function runImportPlace(studio: Studio, opts: ImportOptions, onStatus: (s: string) => void) {
  const { doc, stats } = await importPlace(opts, onStatus);
  if (opts.mode === 'new') studio.addDoc(doc);
  else { doc.history.touch(); studio.emit(); }
  studio.output.system(`Imported "${opts.name}": ${stats.buildings} buildings, ${stats.roads} roads, ${stats.water} water areas${stats.terrain ? ', terrain' : ''}.`);
}

async function git(dir: string, args: string[]) {
  const r = await native!.gitRun({ dir, args });
  return r;
}

export function mapDirOf(doc: Document): string | undefined { return doc.settings.mapDir; }

/** Writes the project into its map folder (asks for one the first time). */
export async function saveMap(studio: Studio, forceAsk = false): Promise<string | null> {
  const doc = studio.active;
  if (!doc || !need(studio)) return null;
  let dir = doc.settings.mapDir;
  if (!dir || forceAsk) {
    dir = (await native!.pickFolder({ title: 'Choose the map folder (a git repository or an empty folder)', create: true })) ?? undefined;
    if (!dir) return null;
    doc.settings.mapDir = dir;
  }
  try {
    const { files, report } = exportMap(doc, mapOptions);
    const res = await native!.writeMapFiles({ dir, files, removeStale: ['tiles'] });
    doc.history.touch();
    studio.output.system(`Map saved to ${dir}: ${report.objects} objects, ${report.roads} roads, ${report.scripts} scripts (${res.written} files changed, ${res.removed} removed)` +
      (report.skippedOsm ? `; ${report.skippedOsm} imported OpenStreetMap objects were left out (OSM is the base world).` : '.'));
    for (const w of report.warnings.slice(0, 20)) studio.output.warn(w);
    return dir;
  } catch (e) { studio.error('Save map', e); return null; }
}

export async function openMapFolder(studio: Studio, dirIn?: string) {
  if (!need(studio)) return;
  const dir = dirIn ?? (await native!.pickFolder({ title: 'Open a worldridr-map folder' }));
  if (!dir) return;
  try {
    const files = await native!.readMapDir(dir);
    const { doc, report } = importMap(files, dir.replace(/^.*[\\/]/, ''));
    doc.settings.mapDir = dir;
    studio.addDoc(doc);
    studio.output.system(`Opened map ${dir}: ${report.objects} objects, ${report.roads} roads${report.terrain ? ', terrain' : ''}.`);
    for (const w of report.warnings.slice(0, 20)) studio.output.warn(w);
  } catch (e) { studio.error('Open map', e); }
}

export async function cloneMap(studio: Studio) {
  if (!need(studio)) return;
  const url = await studio.prompt('Clone map repository', 'Git URL of the map repository (e.g. https://github.com/<owner>/worldridr-world.git):', '');
  if (!url) return;
  const parent = await native!.pickFolder({ title: 'Where should the repository folder be created?', create: true });
  if (!parent) return;
  studio.output.system(`Cloning ${url} …`);
  const r = await git(parent, ['clone', url]);
  if (r.code !== 0) { studio.output.error('git clone failed: ' + r.err); return; }
  const name = url.replace(/\/+$/, '').replace(/\.git$/, '').replace(/^.*[/:]/, '');
  const dir = parent.replace(/[\\/]+$/, '') + '/' + name;
  await openMapFolder(studio, dir);
}

/** Save, commit and push the map. */
export async function commitAndPush(studio: Studio) {
  if (!need(studio)) return;
  const dir = await saveMap(studio);
  if (!dir) return;
  try {
    let r = await git(dir, ['rev-parse', '--is-inside-work-tree']);
    if (r.code !== 0) {
      r = await git(dir, ['init']);
      if (r.code !== 0) throw new Error(r.err);
      studio.output.system('Created a new git repository in the map folder.');
    }
    r = await git(dir, ['status', '--porcelain']);
    if (!r.out.trim()) { studio.output.system('Nothing changed since the last commit.'); }
    else {
      const msg = await studio.prompt('Commit message', 'Describe your change (e.g. "Built the new bridge in Graz"):', 'Update map');
      if (msg === null) return;
      r = await git(dir, ['add', '-A']);
      if (r.code !== 0) throw new Error(r.err);
      r = await git(dir, ['commit', '-m', msg.trim() || 'Update map']);
      if (r.code !== 0) throw new Error(r.err || r.out);
      studio.output.system('Committed: ' + (msg.trim() || 'Update map'));
    }
    r = await git(dir, ['remote']);
    if (!r.out.trim()) {
      const url = await studio.prompt('Remote repository', 'Public git repository to push to (create an empty repo on GitHub first), e.g. https://github.com/<owner>/worldridr-world.git:', '');
      if (!url) { studio.output.warn('Not pushed (no remote). Committed locally.'); return; }
      r = await git(dir, ['remote', 'add', 'origin', url]);
      if (r.code !== 0) throw new Error(r.err);
    }
    studio.output.system('Pushing…');
    r = await git(dir, ['push', '-u', 'origin', 'HEAD']);
    if (r.code !== 0) {
      if (/rejected|non-fast-forward|fetch first/i.test(r.err)) throw new Error('Push rejected: somebody else pushed first. Use "Pull" and then push again.\n' + r.err);
      throw new Error(r.err);
    }
    studio.output.system('Pushed to the shared map repository.');
  } catch (e) { studio.error('Git', e); }
}

/** Pull the latest changes and reopen the map. */
export async function pullMap(studio: Studio) {
  const doc = studio.active;
  if (!need(studio) || !doc) return;
  const dir = doc.settings.mapDir;
  if (!dir) { studio.output.warn('This project has no map folder yet (Map → Save Map).'); return; }
  const st = await git(dir, ['status', '--porcelain']);
  if (st.out.trim()) { studio.output.warn('You have uncommitted changes. Use "Commit & Push" first, then pull.'); return; }
  const r = await git(dir, ['pull', '--no-rebase', '--no-edit']);
  if (r.code !== 0) { studio.output.error('git pull failed: ' + r.err + ' (conflicts: edit the tile files, keep both lines, commit).'); return; }
  studio.output.system(r.out.trim().split('\n')[0] || 'Up to date.');
  await openMapFolder(studio, dir);
}

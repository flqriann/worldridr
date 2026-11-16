// Versioned project/scene/model file format.
//
//   ProjectFile   { format: 'forge-project', version, name, settings, assets[], scene }
//   SceneFile     { format: 'forge-scene',   version, scene, assets[] }
//   ModelFile     { format: 'forge-model',   version, instances[], assets[] }
//
// Instances are stored as a tree of { c: className, id, p: props, a?: attributes,
// t?: terrain, ch?: children }. Unknown classes / invalid values are reported
// as warnings and replaced, never silently dropped.

import { DataModel, Instance } from './Instance';
import { getClass, getPropDef, getProps } from './classes';
import { isUDim2, isVec3, normalizeColor } from './types';
import { TerrainData } from '../terrain/TerrainData';

export const FORMAT_VERSION = 1;

export interface InstanceJSON {
  c: string;
  id?: string;
  p: Record<string, unknown>;
  a?: Record<string, unknown>;
  t?: ReturnType<TerrainData['serialize']>;
  ch?: InstanceJSON[];
}

export interface SceneJSON { services: InstanceJSON[] }

export interface AssetRecord {
  id: string;
  name: string;
  kind: 'model' | 'texture' | 'sound' | 'animation' | 'script' | 'material';
  mime: string;
  /** base64 for binary data, plain text/JSON otherwise */
  data: string;
  encoding: 'base64' | 'text';
  created: number;
}

export interface ProjectSettings {
  gridSize: number;
  moveSnap: number;
  rotateSnap: number;
  snapEnabled: boolean;
  description: string;
  /** Where this project was last published (Forge Server URL, game id, version). The publish key is never stored in the project. */
  publish?: { server: string; gameId: string; version: number };
  /** Folder of the worldridr-map (git repository) this project saves to. */
  mapDir?: string;
}

export const DEFAULT_SETTINGS: ProjectSettings = { gridSize: 4, moveSnap: 1, rotateSnap: 15, snapEnabled: true, description: '' };

export interface ProjectFile {
  format: 'forge-project';
  version: number;
  name: string;
  created: string;
  modified: string;
  settings: ProjectSettings;
  assets: AssetRecord[];
  scene: SceneJSON;
  editor?: { camera?: { position: number[]; yaw: number; pitch: number } };
}

export interface SceneFile { format: 'forge-scene'; version: number; name: string; scene: SceneJSON; assets: AssetRecord[] }
export interface ModelFile { format: 'forge-model'; version: number; name: string; instances: InstanceJSON[]; assets: AssetRecord[] }

export class LoadReport {
  warnings: string[] = [];
  warn(msg: string) { if (this.warnings.length < 200) this.warnings.push(msg); }
}

// ---- Instances ------------------------------------------------------------

export function serializeInstance(inst: Instance, keepIds = true): InstanceJSON {
  const p: Record<string, unknown> = {};
  for (const pd of getProps(inst.className)) {
    if (pd.name === 'ClassName') continue;
    p[pd.name] = inst.props[pd.name];
  }
  const out: InstanceJSON = { c: inst.className, p };
  if (keepIds) out.id = inst.id;
  if (Object.keys(inst.attributes).length) out.a = { ...inst.attributes };
  if (inst.terrain) out.t = inst.terrain.serialize();
  const kids = inst.children.filter((c) => !c.runtimeOnly);
  if (kids.length) out.ch = kids.map((c) => serializeInstance(c, keepIds));
  return out;
}

export function sanitizeValue(className: string, prop: string, value: unknown, report: LoadReport): { ok: boolean; value?: unknown } {
  const pd = getPropDef(className, prop);
  if (!pd) { report.warn(`${className}.${prop}: unknown property ignored`); return { ok: false }; }
  const bad = () => { report.warn(`${className}.${prop}: invalid value replaced by default`); return { ok: false }; };
  switch (pd.type) {
    case 'number': return typeof value === 'number' && Number.isFinite(value) ? { ok: true, value } : bad();
    case 'int': return typeof value === 'number' && Number.isFinite(value) ? { ok: true, value: Math.round(value) } : bad();
    case 'bool': return typeof value === 'boolean' ? { ok: true, value } : bad();
    case 'string': case 'source': case 'asset': return typeof value === 'string' ? { ok: true, value } : bad();
    case 'vec3': return isVec3(value) && [value.x, value.y, value.z].every(Number.isFinite)
      ? { ok: true, value: { x: value.x, y: value.y, z: value.z } } : bad();
    case 'udim2': return isUDim2(value) ? { ok: true, value: { xs: value.xs, xo: value.xo, ys: value.ys, yo: value.yo } } : bad();
    case 'color': { const c = typeof value === 'string' ? normalizeColor(value) : null; return c ? { ok: true, value: c } : bad(); }
    case 'enum': return typeof value === 'string' && pd.enumValues!.includes(value) ? { ok: true, value } : bad();
  }
}

export function deserializeInstance(json: InstanceJSON, report: LoadReport, keepIds = true): Instance | null {
  const def = getClass(json?.c);
  if (!def || def.abstract) {
    report.warn(`Unknown class "${json?.c}" – replaced by a Folder`);
    const f = new Instance('Folder');
    f.props.Name = typeof json?.p?.Name === 'string' ? json.p.Name : 'Unknown';
    attachChildren(f, json, report, keepIds);
    return f;
  }
  const inst = new Instance(json.c, keepIds && typeof json.id === 'string' ? json.id : undefined);
  for (const [k, v] of Object.entries(json.p ?? {})) {
    if (k === 'ClassName') continue;
    const r = sanitizeValue(json.c, k, v, report);
    if (r.ok) inst.props[k] = r.value;
  }
  if (json.a && typeof json.a === 'object') inst.attributes = { ...json.a };
  if (json.c === 'Terrain') {
    try { inst.terrain = json.t ? TerrainData.deserialize(json.t) : new TerrainData(); }
    catch (e) { report.warn(`Terrain: ${(e as Error).message}; terrain reset`); inst.terrain = new TerrainData(); }
  }
  attachChildren(inst, json, report, keepIds);
  return inst;
}

function attachChildren(inst: Instance, json: InstanceJSON, report: LoadReport, keepIds: boolean) {
  for (const cj of Array.isArray(json?.ch) ? json.ch : []) {
    const c = deserializeInstance(cj, report, keepIds);
    if (c) { c.parent = inst; inst.children.push(c); }
  }
}

// ---- Scenes ---------------------------------------------------------------

export function serializeScene(dm: DataModel): SceneJSON {
  return { services: dm.children.filter((c) => !c.runtimeOnly).map((c) => serializeInstance(c)) };
}

const REQUIRED_SERVICES = ['Workspace', 'Lighting', 'Players', 'ReplicatedStorage', 'ServerScriptService', 'ServerStorage', 'StarterGui', 'StarterPlayer', 'SoundService'];

/** Builds a DataModel from scene JSON; missing services are recreated. */
export function deserializeScene(scene: SceneJSON, report: LoadReport, keepIds = true): DataModel {
  const dm = new DataModel();
  for (const sj of Array.isArray(scene?.services) ? scene.services : []) {
    const s = deserializeInstance(sj, report, keepIds);
    if (!s) continue;
    if (!getClass(s.className)?.locked) { report.warn(`Top-level ${s.className} moved into Workspace`); }
    s.setParent(dm);
  }
  ensureServices(dm);
  // move stray non-service top-level objects into Workspace
  for (const c of [...dm.children]) if (!getClass(c.className)?.locked) c.setParent(dm.workspace);
  return dm;
}

export function ensureServices(dm: DataModel) {
  for (const s of REQUIRED_SERVICES) {
    if (!dm.getService(s)) {
      const inst = new Instance(s);
      inst.props.Name = s;
      inst.setParent(dm);
    }
  }
  const sp = dm.getService('StarterPlayer')!;
  if (!sp.findFirstChildOfClass('StarterPlayerScripts')) {
    const s = new Instance('StarterPlayerScripts'); s.props.Name = 'StarterPlayerScripts'; s.setParent(sp);
  }
  const ws = dm.workspace;
  if (!ws.findFirstChildOfClass('Terrain')) {
    const t = new Instance('Terrain'); t.props.Name = 'Terrain'; t.terrain = new TerrainData(); t.setParent(ws, 0);
  }
  const t = ws.findFirstChildOfClass('Terrain')!;
  if (!t.terrain) t.terrain = new TerrainData();
}

// ---- Files ----------------------------------------------------------------

export type AnyFile = ProjectFile | SceneFile | ModelFile;

/** Parses and migrates any supported file. Throws a readable error on failure. */
export function parseFile(text: string): AnyFile {
  let raw: any;
  try { raw = JSON.parse(text); } catch { throw new Error('The file is not valid JSON – it may be damaged or not a Forge file.'); }
  if (!raw || typeof raw !== 'object' || typeof raw.format !== 'string') throw new Error('Not a Forge Studio file (missing "format").');
  if (!['forge-project', 'forge-scene', 'forge-model'].includes(raw.format)) throw new Error(`Unsupported file format "${raw.format}".`);
  const v = Number(raw.version);
  if (!Number.isFinite(v)) throw new Error('File has no version number.');
  if (v > FORMAT_VERSION) throw new Error(`This file was created by a newer Forge Studio (format v${v}); this version supports v${FORMAT_VERSION}.`);
  return migrate(raw);
}

/** Upgrades older format versions in place. v1 is the first public format. */
function migrate(raw: any): AnyFile {
  // Future: if (raw.version < 2) { ...transform...; raw.version = 2 }
  if (!Array.isArray(raw.assets)) raw.assets = [];
  if (raw.format === 'forge-project') raw.settings = { ...DEFAULT_SETTINGS, ...(raw.settings ?? {}) };
  return raw as AnyFile;
}

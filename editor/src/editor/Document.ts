// An open project: scene (DataModel), undo history, selection, assets,
// settings, file location and the set of open script tabs.

import { DataModel, Instance } from '../core/Instance';
import { History } from '../core/History';
import { Signal } from '../core/Signal';
import { DEFAULT_SETTINGS, FORMAT_VERSION, LoadReport, deserializeScene, ensureServices, serializeScene, type ProjectFile, type ProjectSettings, type SceneFile } from '../core/Serializer';
import { newId } from '../core/types';
import { AssetStore } from '../assets/AssetStore';
import { Selection } from './Selection';

export interface CameraState { position: [number, number, number]; yaw: number; pitch: number }

export class Document {
  readonly id = newId('doc-');
  name: string;
  filePath: string | null = null;
  /** Key in the project library ("My Projects"), or null for loose files / unsaved projects. */
  libraryKey: string | null = null;
  /** Last captured viewport thumbnail (data URL) for the start page. */
  thumbnail: string | null = null;
  dm: DataModel;
  readonly history = new History();
  readonly selection = new Selection();
  assets: AssetStore;
  settings: ProjectSettings;
  created: string;
  /** Script instance ids open as tabs, in order. */
  openScripts: string[] = [];
  /** 'scene' or a script instance id. */
  activeTab = 'scene';
  camera: CameraState | null = null;
  readonly changed = new Signal();

  constructor(name: string, dm: DataModel, assets = new AssetStore(), settings: ProjectSettings = { ...DEFAULT_SETTINGS }) {
    this.name = name;
    this.dm = dm;
    this.assets = assets;
    this.settings = settings;
    this.created = new Date().toISOString();
    ensureServices(dm);
    dm.history = this.history;
    this.selection.bind(dm);
    this.history.changed.connect(() => this.changed.fire());
    this.assets.changed.connect(() => { this.history.touch(); });
    // close script tabs whose scripts were deleted
    dm.removed.connect((inst) => {
      if (this.openScripts.includes(inst.id)) this.closeScript(inst.id);
    });
  }

  get dirty() { return this.history.isDirty; }
  get title() { return this.name + (this.dirty ? ' •' : ''); }

  transact<T>(label: string, fn: () => T, coalesceKey?: string): T {
    return this.history.transact(label, fn, coalesceKey);
  }

  openScript(inst: Instance) {
    if (!this.openScripts.includes(inst.id)) this.openScripts.push(inst.id);
    this.activeTab = inst.id;
    this.changed.fire();
  }

  closeScript(id: string) {
    const i = this.openScripts.indexOf(id);
    if (i < 0) return;
    this.openScripts.splice(i, 1);
    if (this.activeTab === id) this.activeTab = this.openScripts[Math.min(i, this.openScripts.length - 1)] ?? 'scene';
    this.changed.fire();
  }

  setActiveTab(tab: string) { if (this.activeTab !== tab) { this.activeTab = tab; this.changed.fire(); } }

  rename(name: string) { this.name = name; this.history.touch(); }

  toProjectFile(): ProjectFile {
    const now = new Date().toISOString();
    return {
      format: 'forge-project',
      version: FORMAT_VERSION,
      name: this.name,
      created: this.created,
      modified: now,
      settings: { ...this.settings },
      assets: this.assets.toJSON(),
      scene: serializeScene(this.dm),
      editor: this.camera ? { camera: { position: [...this.camera.position], yaw: this.camera.yaw, pitch: this.camera.pitch } } : undefined,
    };
  }

  toSceneFile(): SceneFile {
    return { format: 'forge-scene', version: FORMAT_VERSION, name: this.name, scene: serializeScene(this.dm), assets: this.assets.toJSON() };
  }

  static fromProjectFile(f: ProjectFile, report: LoadReport): Document {
    const dm = deserializeScene(f.scene, report);
    const doc = new Document(typeof f.name === 'string' ? f.name : 'Untitled', dm, new AssetStore(f.assets), { ...DEFAULT_SETTINGS, ...f.settings });
    if (typeof f.created === 'string') doc.created = f.created;
    const cam = f.editor?.camera;
    if (cam && Array.isArray(cam.position) && cam.position.length === 3) doc.camera = { position: cam.position as [number, number, number], yaw: cam.yaw, pitch: cam.pitch };
    doc.history.markSaved();
    return doc;
  }

  static fromSceneFile(f: SceneFile, report: LoadReport): Document {
    const dm = deserializeScene(f.scene, report);
    const doc = new Document(typeof f.name === 'string' ? f.name : 'Scene', dm, new AssetStore(f.assets));
    doc.history.markSaved();
    return doc;
  }
}

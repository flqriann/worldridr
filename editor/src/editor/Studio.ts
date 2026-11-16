// Central editor controller: open documents (tabs), tools, clipboard,
// commands (file/edit/insert/test), dialogs, autosave and play sessions.

import * as THREE from 'three';
import { Signal } from '../core/Signal';
import { Instance } from '../core/Instance';
import { creatableClasses, getClass } from '../core/classes';
import { markToolboxUsed, toolboxItem, type ToolboxItem } from './toolbox';
import { SCRIPT_TEMPLATES } from '../assets/builtin';
import { FORMAT_VERSION, LoadReport, deserializeInstance, parseFile, serializeInstance, type AssetRecord, type InstanceJSON, type ModelFile } from '../core/Serializer';
import { udim2, v3, type Vec3 } from '../core/types';
import { Document } from './Document';
import { OutputLog } from './Output';
import type { Tool, Viewport } from '../render/Viewport';
import { boundsOf, collectTransformables, quatToOrientation } from '../render/geometry';
import { addRecent, autosave, getRecent, isElectron, native, openFiles, readPath, removeRecent, saveFile, type FileKind } from '../project/ProjectIO';
import type { ServerInfo } from '../project/ProjectIO';
import { createExample, createFromTemplate, type TemplateId } from '../project/templates';
import { buildPrefab } from '../assets/prefabs';
import { assetBytes, EXT_BY_MIME } from '../assets/AssetStore';
import { modelNaturalSize } from '../render/models';
import type { PlaySession } from '../runtime/PlaySession';
import { History } from '../core/History';
import type { TerrainSnapshot } from '../terrain/TerrainData';
import { LuaRuntime } from '../runtime/LuaRuntime';
import { library } from '../project/library';
import { ServerApi, normalizeServerUrl, type VersionInfo } from '../net/api';
import { t } from '../i18n';
import { runSelfTest } from './selfTest';

History.applyTerrain = (inst, snap) => {
  if (!inst.terrain) return;
  const s = snap as TerrainSnapshot;
  inst.terrain.restore(s);
  inst.dm?.terrainChanged.fire(inst, s.region ?? null);
};

export interface DialogButton { id: string; label: string; primary?: boolean; danger?: boolean }
export interface DialogRequest {
  title: string;
  message: string;
  buttons: DialogButton[];
  input?: { value: string; placeholder?: string };
  resolve: (r: { button: string; value?: string }) => void;
}

export interface ContextMenuItem { label?: string; shortcut?: string; action?: () => void; disabled?: boolean; separator?: boolean; children?: ContextMenuItem[]; checked?: boolean }

const ASSET_PROPS = ['MeshId', 'TextureId', 'SoundId', 'AnimationId', 'Image'];
const PLACEABLE_IN_PART = new Set(['PointLight', 'SpotLight', 'ParticleEmitter', 'ClickDetector', 'KeyframeAnimation']);
const GUI_CLASSES = new Set(['Frame', 'TextLabel', 'TextButton', 'ImageLabel']);

export class Studio {
  docs: Document[] = [];
  active: Document | null = null;
  /** 'start' shows the start page instead of a document. */
  showStart = true;
  tool: Tool = 'select';
  readonly output = new OutputLog();
  viewport: Viewport | null = null;
  play: PlaySession | null = null;
  clipboard: { instances: InstanceJSON[]; assets: AssetRecord[] } | null = null;
  autosaveMinutes = 2;
  showGrid = true;
  showStats = false;
  /** Bumped on any state change relevant to menus/toolbars. */
  readonly changed = new Signal();
  readonly dialogs = new Signal<[DialogRequest]>();
  readonly contextMenu = new Signal<[{ x: number; y: number; items: ContextMenuItem[] }]>();
  /** Requests the Explorer to start renaming an instance. */
  readonly renameRequest = new Signal<[Instance]>();
  /** Requests a panel to be shown/focused (e.g. 'output', 'assets'). */
  readonly panelRequest = new Signal<[string]>();
  private fileHandles = new Map<string, FileSystemFileHandle>();
  private docUnsub = new Map<string, () => void>();
  private autosaveTimer: number | undefined;
  /** Factory provided by the runtime module (avoids a static import cycle). */
  static createPlaySession: (studio: Studio, doc: Document) => PlaySession;

  constructor() {
    try {
      const stored = localStorage.getItem('forge.autosaveMinutes');
      if (stored !== null && Number.isFinite(Number(stored))) this.autosaveMinutes = Math.max(0, Number(stored));
    } catch { /* storage unavailable: keep default */ }
    this.scheduleAutosave();
  }

  // ---- Documents ---------------------------------------------------------------

  attachViewport(vp: Viewport) {
    this.viewport = vp;
    vp.input = {
      onContextMenu: (x, y, target) => this.showContextMenu(x, y, target, 'viewport'),
      onMessage: (l, m) => this.output.log(l, m),
      onToolboxDrop: (id, point) => { const item = toolboxItem(id); if (item) this.useToolboxItem(item, point); },
      onFilesDrop: (files, point) => { this.dropFiles(files, point); },
    };
    vp.setTool(this.tool);
    vp.showGrid = this.showGrid;
    this.bindViewport();
  }

  private bindViewport() {
    const vp = this.viewport;
    if (!vp) return;
    if (this.play) return; // play session owns the viewport
    const d = this.active;
    vp.setDataModel(d?.dm ?? null, d, d?.assets ?? null, 'edit');
    vp.gui.setEditing(this.uiEditor && d ? d : null);
    this.selectionUnsub?.();
    // selecting a GUI element switches the on-screen UI editor on (like Roblox Studio)
    this.selectionUnsub = d?.selection.changed.connect(() => {
      if (!this.uiEditor && !this.play && d.selection.primary?.isA('GuiObject')) { this.uiEditor = true; vp.gui.setEditing(d); this.emit(); }
    }) ?? null;
  }

  private selectionUnsub: (() => void) | null = null;

  addDoc(doc: Document, activate = true) {
    this.docs.push(doc);
    this.docUnsub.set(doc.id, doc.changed.connect(() => this.emit()));
    if (activate) this.activate(doc);
    else this.emit();
  }

  activate(doc: Document | null) {
    if (this.play && doc !== this.active) { this.output.warn(t('Stop the running test before switching projects.')); return; }
    if (this.active && this.viewport) this.viewport.saveCameraTo(this.active);
    this.active = doc;
    this.showStart = !doc;
    this.bindViewport();
    this.updateTitle();
    this.emit();
  }

  showStartPage() {
    if (this.play) return;
    if (this.active && this.viewport) this.viewport.saveCameraTo(this.active);
    this.showStart = true;
    this.emit();
  }

  emit() { this.changed.fire(); this.updateTitle(); }

  private updateTitle() {
    const t = this.active ? `${this.active.title} – WorldRidr Studio` : 'WorldRidr Studio';
    document.title = t;
    native?.setTitle(t);
  }

  async closeDoc(doc: Document): Promise<boolean> {
    if (this.play && this.active === doc) await this.stopPlay();
    if (doc.dirty) {
      const r = await this.ask(t('Unsaved changes'), t('Save changes to "{name}" before closing?', { name: doc.name }), [
        { id: 'save', label: t('Save'), primary: true }, { id: 'discard', label: t("Don't save"), danger: true }, { id: 'cancel', label: t('Cancel') },
      ]);
      if (r.button === 'cancel') return false;
      if (r.button === 'save' && !(await this.save(doc))) return false;
    }
    const i = this.docs.indexOf(doc);
    this.docs.splice(i, 1);
    this.docUnsub.get(doc.id)?.();
    this.docUnsub.delete(doc.id);
    this.fileHandles.delete(doc.id);
    autosave.remove(doc.id).catch(() => {});
    if (this.active === doc) this.activate(this.docs[Math.min(i, this.docs.length - 1)] ?? null);
    else this.emit();
    return true;
  }

  newProject(template: TemplateId = 'baseplate') {
    const doc = createFromTemplate(template);
    let n = 1;
    while (this.docs.some((d) => d.name === `${doc.name} ${n}`)) n++;
    doc.name = `${doc.name} ${n}`;
    doc.history.markSaved();
    this.addDoc(doc);
    this.output.system(t('Created new project "{name}".', { name: doc.name }));
  }

  openExample(id: string) {
    const doc = createExample(id);
    this.addDoc(doc);
    this.output.system(t('Opened example "{name}". Press Play (F5) to test it.', { name: t(doc.name) }));
  }

  async openProjectDialog() {
    try {
      const files = await openFiles('project');
      for (const f of files) this.loadProjectData(f.name, new TextDecoder().decode(f.data), f.path, f.handle);
    } catch (e) { this.error(t('Open failed'), e); }
  }

  async openPath(path: string) {
    try {
      const data = await readPath(path);
      this.loadProjectData(path.replace(/^.*[\\/]/, ''), new TextDecoder().decode(data), path);
    } catch (e) {
      removeRecent(path);
      this.error(t('Could not open "{name}"', { name: path }), e);
    }
  }

  /** Parses project/scene text and opens it in a new tab. Returns the document. */
  loadProjectData(fileName: string, text: string, path: string | null, handle?: FileSystemFileHandle): Document | null {
    const existing = path ? this.docs.find((d) => d.filePath === path) : undefined;
    if (existing) { this.activate(existing); return existing; }
    const parsed = parseFile(text);
    const report = new LoadReport();
    let doc: Document;
    if (parsed.format === 'forge-project') {
      doc = Document.fromProjectFile(parsed, report);
      doc.filePath = path ?? (handle ? handle.name : null);
    } else if (parsed.format === 'forge-scene') {
      doc = Document.fromSceneFile(parsed, report);
    } else {
      throw new Error('This is a model file. Use Insert ▸ Model from File to add it to a scene.');
    }
    if (handle) this.fileHandles.set(doc.id, handle);
    this.addDoc(doc);
    if (path) addRecent(path, doc.name);
    this.output.system(t('Opened "{name}" ({n} objects, {a} assets).', { name: fileName, n: doc.dm.index.size, a: doc.assets.list().length }));
    for (const w of report.warnings) this.output.warn(t('Load: {msg}', { msg: w }));
    return doc;
  }

  /**
   * Saves a project. Projects opened from a file are written back to that file;
   * new projects go into the project library (no dialog). `toFile` always asks
   * for a file location ("Save to File…").
   */
  async save(doc = this.active, toFile = false): Promise<boolean> {
    if (!doc) return false;
    if (this.play && this.active === doc) this.output.warn(t('Saving the edit version of the scene (changes made during the test are not saved).'));
    try {
      if (this.viewport && this.active === doc) this.viewport.saveCameraTo(doc);
      const thumbnail = this.active === doc && !this.play ? this.viewport?.captureThumbnail() ?? doc.thumbnail : doc.thumbnail;
      doc.thumbnail = thumbnail;
      const text = JSON.stringify(doc.toProjectFile());
      const kb = (text.length / 1024).toFixed(1);
      const handle = toFile ? undefined : this.fileHandles.get(doc.id);

      if (!toFile && !handle && (doc.libraryKey || !doc.filePath)) {
        // project library (desktop: Documents/Forge Studio/Projects, browser: IndexedDB)
        doc.libraryKey ??= await library.newKey(doc.name);
        await library.save(doc.libraryKey, doc.name, text, thumbnail);
        if (isElectron) { doc.filePath = doc.libraryKey; addRecent(doc.libraryKey, doc.name); }
        doc.history.markSaved();
        await autosave.remove(doc.id).catch(() => {});
        this.output.system(t('Saved "{name}" to My Projects ({kb} KB).', { name: doc.name, kb }));
        this.emit();
        return true;
      }

      const path = toFile ? null : (isElectron ? doc.filePath : null);
      const res = await saveFile('project', `${doc.name.replace(/[\\/:*?"<>|]/g, '_')}.forge`, text, { path, handle });
      if (!res) return false;
      if (res.handle) this.fileHandles.set(doc.id, res.handle);
      if (res.path) { doc.filePath = res.path; addRecent(res.path, doc.name); doc.libraryKey = library.isLibraryKey(res.path) ? res.path : null; }
      else if (res.handle) { doc.filePath = res.handle.name; doc.libraryKey = null; }
      if (toFile) {
        const n = res.name.replace(/\.forge$/i, '');
        if (n) doc.name = n;
      }
      doc.history.markSaved();
      await autosave.remove(doc.id).catch(() => {});
      this.output.system(res.path || res.handle
        ? t('Saved "{name}" ({kb} KB).', { name: res.name, kb })
        : t('Downloaded "{name}" ({kb} KB) – the browser cannot write files directly.', { name: res.name, kb }));
      this.emit();
      return true;
    } catch (e) {
      this.error(t('Save failed'), e);
      return false;
    }
  }

  /** Opens a project from the library (start page). */
  async openLibraryProject(key: string, name: string) {
    const existing = this.docs.find((d) => d.libraryKey === key || d.filePath === key);
    if (existing) { this.activate(existing); return; }
    try {
      const text = await library.read(key);
      const doc = this.loadProjectData(name, text, isElectron ? key : null);
      if (doc) { doc.libraryKey = key; doc.name = name; doc.history.markSaved(); this.emit(); }
    } catch (e) { this.error(t('Could not open "{name}"', { name }), e); }
  }

  async exportScene() {
    const doc = this.active;
    if (!doc) return;
    try {
      const res = await saveFile('scene', `${doc.name}.forgescene`, JSON.stringify(doc.toSceneFile()));
      if (res) this.output.system(t('Exported scene to "{name}".', { name: res.name }));
    } catch (e) { this.error(t('Export failed'), e); }
  }

  async exportSelection() {
    const doc = this.active;
    if (!doc || !doc.selection.size) { this.output.warn(t('Select objects to export first.')); return; }
    const list = doc.selection.list.filter((i) => !i.locked);
    const file = this.makeModelFile(doc, list, list.length === 1 ? list[0].name : 'Model');
    try {
      const res = await saveFile('model', `${file.name}.forgemodel`, JSON.stringify(file));
      if (res) this.output.system(t('Exported {n} object(s) to "{name}".', { n: list.length, name: res.name }));
    } catch (e) { this.error(t('Export failed'), e); }
  }

  makeModelFile(doc: Document, list: readonly Instance[], name: string): ModelFile {
    const ids = new Set<string>();
    for (const i of list) i.walk((d) => { for (const p of ASSET_PROPS) { const v = d.props[p]; if (typeof v === 'string' && v && doc.assets.has(v)) ids.add(v); } });
    return {
      format: 'forge-model', version: FORMAT_VERSION, name,
      instances: list.map((i) => serializeInstance(i, false)),
      assets: [...ids].map((id) => doc.assets.get(id)!).filter(Boolean),
    };
  }

  async exportGLB() {
    const vp = this.viewport;
    const doc = this.active;
    if (!vp || !doc) return;
    try {
      const { GLTFExporter } = await import('three/examples/jsm/exporters/GLTFExporter.js');
      const scene = new THREE.Scene();
      const ws = doc.dm.workspace;
      const parts = doc.selection.size ? collectTransformables(doc.selection.list) : ws.getDescendants().filter((d) => d.isA('BasePart'));
      for (const p of parts) {
        if (p.props.Locked && !doc.selection.size) continue;
        const c = vp.exportObject(p);
        if (!c) continue;
        c.traverse((o) => { if ((o as THREE.LineSegments).isLineSegments || (o as THREE.Light).isLight) o.visible = false; });
        c.name = p.name;
        scene.add(c);
      }
      const terr = doc.dm.terrainInstance;
      const exporter = new GLTFExporter();
      const glb = await exporter.parseAsync(scene, { binary: true, onlyVisible: true }) as ArrayBuffer;
      const res = await saveFile('glb', `${doc.name}.glb`, glb);
      if (res) this.output.system(t('Exported {n} parts as glTF binary "{name}" ({kb} KB).', { n: scene.children.length, name: res.name, kb: (glb.byteLength / 1024).toFixed(0) }) + (terr?.terrain?.hasSolid() ? ' ' + t('Terrain is not included in GLB export.') : ''));
    } catch (e) { this.error(t('GLB export failed'), e); }
  }

  /** Inserts objects from a .forgemodel file. */
  async importModelFile() {
    const doc = this.active;
    if (!doc) return;
    try {
      const files = await openFiles('model', true);
      for (const f of files) {
        const parsed = parseFile(new TextDecoder().decode(f.data));
        if (parsed.format !== 'forge-model') throw new Error(`${f.name} is not a model file.`);
        this.insertModelFile(doc, parsed);
      }
    } catch (e) { this.error(t('Import failed'), e); }
  }

  insertModelFile(doc: Document, file: ModelFile, at?: Vec3): Instance[] {
    for (const a of file.assets ?? []) if (!doc.assets.has(a.id)) doc.assets.add(a);
    const report = new LoadReport();
    const insts = (file.instances ?? []).map((j) => deserializeInstance(j, report, false)).filter((x): x is Instance => !!x);
    for (const w of report.warnings) this.output.warn(t('Import: {msg}', { msg: w }));
    return this.placeInstances(doc, insts, `Insert ${file.name}`, at);
  }

  /** Parents detached instances into the scene (moved to the insert point), selects them. */
  placeInstances(doc: Document, insts: Instance[], label: string, at?: Vec3, parent?: Instance): Instance[] {
    if (!insts.length) return [];
    const parts = insts.flatMap((i) => (i.isA('BasePart') ? [i] : i.getDescendants().filter((d) => d.isA('BasePart'))));
    if (parts.length && this.viewport) {
      const b = boundsOf(parts);
      const size = b.getSize(new THREE.Vector3());
      const target = at ?? this.viewport.insertPosition(v3(size.x, size.y, size.z));
      const c = b.getCenter(new THREE.Vector3());
      const dx = target.x - c.x, dy = target.y - c.y, dz = target.z - c.z;
      for (const p of parts) { const pos = p.props.Position as Vec3; p.props.Position = v3(r3(pos.x + dx), r3(pos.y + dy), r3(pos.z + dz)); }
    }
    const container = parent ?? this.defaultContainer(doc, insts[0]);
    doc.transact(label, () => { for (const i of insts) i.setParent(container); });
    doc.selection.set(insts);
    return insts;
  }

  // ---- Autosave ----------------------------------------------------------------

  setAutosaveMinutes(m: number) {
    this.autosaveMinutes = m;
    try { localStorage.setItem('forge.autosaveMinutes', String(m)); } catch { /* ignore */ }
    this.scheduleAutosave();
    this.emit();
  }

  private scheduleAutosave() {
    clearInterval(this.autosaveTimer);
    if (this.autosaveMinutes > 0) this.autosaveTimer = window.setInterval(() => this.runAutosave(), this.autosaveMinutes * 60_000);
  }

  async runAutosave(): Promise<number> {
    let n = 0;
    for (const d of this.docs) {
      if (!d.dirty) continue;
      try {
        await autosave.write(d.id, JSON.stringify({ ...d.toProjectFile(), autosave: { filePath: d.filePath, docName: d.name } }));
        n++;
      } catch (e) { this.output.warn(t('Autosave of "{name}" failed: {error}', { name: d.name, error: (e as Error).message })); }
    }
    if (n) this.output.system(t('Autosaved {n} project(s).', { n }));
    return n;
  }

  async checkRecovery() {
    const list = await autosave.list();
    if (!list.length) return;
    const r = await this.ask(t('Recover unsaved work'), t('Forge Studio found {n} autosaved project(s) from a previous session that were not saved. Recover them?', { n: list.length }), [
      { id: 'recover', label: t('Recover'), primary: true }, { id: 'discard', label: t('Discard'), danger: true }, { id: 'later', label: t('Ask later') },
    ]);
    if (r.button === 'later') return;
    for (const e of list) {
      if (r.button === 'recover') {
        try {
          const doc = this.loadProjectData('autosave', e.text, null);
          if (doc) {
            const meta = JSON.parse(e.text).autosave;
            doc.name = (meta?.docName ?? doc.name) + ' ' + t('(recovered)');
            doc.filePath = meta?.filePath && isElectron ? meta.filePath : null;
            doc.history.touch();
          }
        } catch (err) { this.error(t('Recovery failed'), err); }
      }
      await autosave.remove(e.id).catch(() => {});
    }
  }

  recentFiles() { return isElectron ? getRecent() : []; }

  // ---- Dialog helpers ---------------------------------------------------------------

  ask(title: string, message: string, buttons: DialogButton[], input?: { value: string; placeholder?: string }): Promise<{ button: string; value?: string }> {
    return new Promise((resolve) => this.dialogs.fire({ title, message, buttons, input, resolve }));
  }

  async prompt(title: string, message: string, value: string): Promise<string | null> {
    const r = await this.ask(title, message, [{ id: 'ok', label: t('OK'), primary: true }, { id: 'cancel', label: t('Cancel') }], { value });
    return r.button === 'ok' ? (r.value ?? '') : null;
  }

  error(context: string, e: unknown) {
    const msg = e instanceof Error ? e.message : String(e);
    this.output.error(`${context}: ${msg}`);
    this.ask(context, msg, [{ id: 'ok', label: t('OK'), primary: true }]);
  }

  // ---- Tools ------------------------------------------------------------------------

  setTool(t: Tool) {
    this.tool = t;
    this.viewport?.setTool(t);
    if (t === 'terrain') this.panelRequest.fire('terrain');
    this.emit();
  }

  toggleGrid() { this.showGrid = !this.showGrid; if (this.viewport) { this.viewport.showGrid = this.showGrid; this.viewport.updateGrid(); } this.emit(); }

  // ---- Edit commands ------------------------------------------------------------------

  /** The DataModel being edited (the play copy while testing). */
  get editDoc(): Document | null { return this.play ? null : this.active; }

  undo() {
    const d = this.editDoc; if (!d) return;
    const l = d.history.undo();
    if (l) this.output.log('system', t('Undo: {action}', { action: l }));
  }
  redo() {
    const d = this.editDoc; if (!d) return;
    const l = d.history.redo();
    if (l) this.output.log('system', t('Redo: {action}', { action: l }));
  }

  private editable(list: readonly Instance[]) { return list.filter((i) => !i.locked); }

  copy() {
    const d = this.active; if (!d) return;
    const list = this.topLevel(this.editable(d.selection.list));
    if (!list.length) return;
    const file = this.makeModelFile(d, list, 'Clipboard');
    this.clipboard = { instances: file.instances, assets: file.assets };
    this.emit();
  }

  cut() { this.copy(); this.deleteSelection('Cut'); }

  paste(into = false) {
    const d = this.editDoc;
    if (!d || !this.clipboard) return;
    const report = new LoadReport();
    for (const a of this.clipboard.assets) if (!d.assets.has(a.id)) d.assets.add(a);
    const insts = this.clipboard.instances.map((j) => deserializeInstance(j, report, false)).filter((x): x is Instance => !!x);
    const target = into ? d.selection.primary : undefined;
    if (into && target) {
      d.transact('Paste Into', () => insts.forEach((i) => i.setParent(target)));
      d.selection.set(insts);
    } else {
      // paste at the same position, offset upward like Roblox when overlapping the original
      const parts = insts.flatMap((i) => (i.isA('BasePart') ? [i] : i.getDescendants().filter((x) => x.isA('BasePart'))));
      if (parts.length) {
        const b = boundsOf(parts);
        const h = b.getSize(new THREE.Vector3()).y;
        for (const p of parts) { const pos = p.props.Position as Vec3; p.props.Position = v3(pos.x, r3(pos.y + h), pos.z); }
      }
      const parent = d.selection.primary?.parent && !d.selection.primary.locked ? d.selection.primary.parent : this.defaultContainer(d, insts[0]);
      d.transact('Paste', () => insts.forEach((i) => i.setParent(parent)));
      d.selection.set(insts);
    }
  }

  duplicate() {
    const d = this.editDoc; if (!d) return;
    const list = this.topLevel(this.editable(d.selection.list));
    if (!list.length) return;
    const clones = list.map((i) => i.clone());
    d.transact('Duplicate', () => clones.forEach((c, k) => c.setParent(list[k].parent)));
    d.selection.set(clones);
  }

  deleteSelection(label = 'Delete') {
    const d = this.editDoc; if (!d) return;
    const list = this.topLevel(d.selection.list);
    const locked = list.filter((i) => i.locked);
    if (locked.length) this.output.warn(t('{names} cannot be deleted.', { names: locked.map((i) => i.className).join(', ') }));
    const del = list.filter((i) => !i.locked);
    if (!del.length) return;
    d.transact(label, () => del.forEach((i) => i.setParent(null)));
    d.selection.clear();
  }

  selectAll() {
    const d = this.active; if (!d) return;
    d.selection.set(d.dm.workspace.children.filter((c) => c.className !== 'Terrain' && c.className !== 'Camera' && !c.props.Locked));
  }

  selectChildren() {
    const d = this.active; if (!d) return;
    d.selection.set(d.selection.list.flatMap((i) => i.children));
  }

  group(className: 'Model' | 'Folder' = 'Model') {
    const d = this.editDoc; if (!d) return;
    const list = this.topLevel(this.editable(d.selection.list));
    if (!list.length) return;
    const parent = list[0].parent!;
    const m = new Instance(className);
    m.props.Name = className;
    d.transact(`Group as ${className}`, () => {
      m.setParent(parent);
      list.forEach((i) => i.setParent(m));
    });
    d.selection.set([m]);
  }

  ungroup() {
    const d = this.editDoc; if (!d) return;
    const groups = d.selection.list.filter((i) => (i.className === 'Model' || i.className === 'Folder') && !i.locked);
    if (!groups.length) { this.output.warn(t('Ungroup: select a Model or Folder.')); return; }
    const freed: Instance[] = [];
    d.transact('Ungroup', () => {
      for (const g of groups) {
        const parent = g.parent!;
        for (const c of [...g.children]) { c.setParent(parent); freed.push(c); }
        g.setParent(null);
      }
    });
    d.selection.set(freed);
  }

  /** Toggles a boolean property on all selected parts (expanding models). */
  toggleProp(prop: 'Anchored' | 'CanCollide' | 'Locked' | 'Visible') {
    const d = this.editDoc; if (!d) return;
    const parts = collectTransformables(d.selection.list).filter((p) => p.isA('BasePart'));
    if (!parts.length) { this.output.warn(t('{prop}: select parts first.', { prop })); return; }
    const value = !parts.every((p) => p.props[prop]);
    d.transact(`${value ? 'Enable' : 'Disable'} ${prop}`, () => parts.forEach((p) => p.set(prop, value)));
  }

  setPropOnSelection(prop: string, value: unknown) {
    const d = this.editDoc; if (!d) return;
    const parts = collectTransformables(d.selection.list).filter((p) => p.isA('BasePart'));
    if (!parts.length) { this.output.warn(t('Select parts to change {prop}.', { prop })); return; }
    d.transact(`Set ${prop}`, () => parts.forEach((p) => p.set(prop, value)));
  }

  rename() { const p = this.active?.selection.primary; if (p && !p.locked) this.renameRequest.fire(p); }

  focusSelection() { if (this.active && this.viewport) this.viewport.focusOn(this.active.selection.list); }

  /** Only the outermost instances of a list (children of selected parents are dropped). */
  topLevel(list: readonly Instance[]): Instance[] {
    return list.filter((i) => !list.some((o) => o !== i && i.isDescendantOf(o)));
  }

  // ---- Insert ----------------------------------------------------------------------

  /**
   * Container for a new instance when nothing explicit was chosen.
   * GUI objects without a ScreenGui get one created – only when `create` is set
   * (i.e. inside the insert transaction, so it is undone together).
   */
  defaultContainer(doc: Document, inst: Instance, create = false): Instance {
    const dm = doc.dm;
    const sel = doc.selection.primary;
    const cn = inst.className;
    const svc = (n: string) => dm.getService(n)!;
    if (cn === 'Sky' || cn === 'Atmosphere') return svc('Lighting');
    if (cn === 'ScreenGui') return svc('StarterGui');
    if (GUI_CLASSES.has(cn)) {
      if (sel && (sel.className === 'ScreenGui' || sel.isA('GuiObject'))) return sel;
      const sg = svc('StarterGui');
      let screen = sg.findFirstChildOfClass('ScreenGui');
      if (!screen && create) { screen = new Instance('ScreenGui'); screen.props.Name = 'ScreenGui'; screen.setParent(sg); }
      return screen ?? sg;
    }
    if (inst.isA('BasePart') || cn === 'Model' || cn === 'Camera') {
      // 3D objects go into a selected Model/Folder in Workspace, otherwise Workspace
      if (sel && (sel.className === 'Model' || sel.className === 'Folder') && sel.isDescendantOf(dm.workspace)) return sel;
      return dm.workspace;
    }
    if (sel && !sel.locked && sel.className !== 'Terrain' && sel.className !== 'Camera') return sel;
    if (sel && ['Workspace', 'ServerScriptService', 'ReplicatedStorage', 'ServerStorage', 'StarterGui', 'SoundService'].includes(sel.className)) return sel;
    if (cn === 'Script') return svc('ServerScriptService');
    if (cn === 'LocalScript') return svc('StarterPlayer').findFirstChildOfClass('StarterPlayerScripts')!;
    if (cn === 'ModuleScript') return svc('ReplicatedStorage');
    if (cn === 'Sound') return svc('SoundService');
    return dm.workspace;
  }

  /** Where a new part of `size` goes: on the dropped surface point, or under the screen center. */
  private placeFor(size: Vec3, at?: Vec3): Vec3 {
    if (at) {
      const snap = this.active?.settings;
      const g = snap?.snapEnabled ? Math.max(snap.moveSnap, 0.001) : 0.001;
      const rs = (x: number) => Math.round(x / g) * g;
      return v3(r3(rs(at.x)), r3(at.y + size.y / 2), r3(rs(at.z)));
    }
    return this.viewport?.insertPosition(size) ?? v3(0, size.y / 2, 0);
  }

  private requireEditDoc(): Document | null {
    const d = this.editDoc;
    if (!d) this.output.warn(this.play ? t('Stop the running test to change the scene.') : t('Open or create a project first.'));
    return d;
  }

  insertObject(className: string, parent?: Instance, at?: Vec3): Instance | null {
    const d = this.requireEditDoc();
    if (!d) return null;
    const def = getClass(className);
    if (!def?.creatable) return null;
    const inst = new Instance(className);
    let created: Instance = inst;
    d.transact(`Insert ${className}`, () => {
      let container = parent ?? this.defaultContainer(d, inst, true);
      if (PLACEABLE_IN_PART.has(className) && !container.isA('BasePart')) {
        if (className === 'ClickDetector' || className === 'KeyframeAnimation') {
          this.output.warn(t('{cls} only works inside a part – it was added to a new part. Select a part first to add it there.', { cls: className }));
        }
        container = this.makeHolderPart(d, className, at);
        created = container;
      }
      if (inst.isA('BasePart')) {
        inst.props.Position = this.placeFor(inst.props.Size as Vec3, at);
        if (className === 'Part' || className === 'WedgePart') inst.props.Anchored = true;
      }
      if (className === 'Camera' && this.viewport) {
        const cam = this.viewport.camera;
        inst.props.Position = v3(r3(cam.position.x), r3(cam.position.y), r3(cam.position.z));
        inst.props.Orientation = quatToOrientation(cam.quaternion);
      }
      inst.setParent(container);
    });
    d.selection.set([created]);
    if (inst.isA('LuaSourceContainer')) this.openScript(inst);
    return inst;
  }

  private makeHolderPart(d: Document, className: string, at?: Vec3, partProps: Record<string, unknown> = {}): Instance {
    const holder = new Instance('Part');
    holder.props.Name = className === 'PointLight' || className === 'SpotLight' ? 'Lamp' : className === 'ParticleEmitter' ? 'Effect' : 'Part';
    holder.props.Size = v3(1, 1, 1);
    holder.props.Anchored = true;
    if (className === 'PointLight' || className === 'SpotLight') { holder.props.Material = 'Neon'; holder.props.Color = '#fff2c4'; holder.props.CastShadow = false; }
    if (className === 'ParticleEmitter') { holder.props.Transparency = 1; holder.props.CanCollide = false; holder.props.CastShadow = false; }
    Object.assign(holder.props, partProps);
    holder.props.Position = this.placeFor(holder.props.Size as Vec3, at);
    holder.setParent(d.dm.workspace);
    return holder;
  }

  /** The part the user is working on: a selected part, or the part containing the selected object. */
  selectedPart(): Instance | undefined {
    const d = this.active;
    if (!d) return undefined;
    for (const i of [...d.selection.list].reverse()) {
      let p: Instance | null = i;
      while (p && !p.isA('BasePart')) p = p.parent;
      if (p && !p.locked && !p.props.Locked && p.dm) return p;
    }
    return undefined;
  }

  /** Lights/effects: into the selected part, or into a new holder part (always when `newPart` or dropped). */
  insertIntoPart(className: string, props: Record<string, unknown>, at?: Vec3, newPart = false, partProps: Record<string, unknown> = {}): Instance[] {
    const d = this.requireEditDoc();
    if (!d) return [];
    const part = this.selectedPart();
    const inst = new Instance(className);
    Object.assign(inst.props, props);
    let holder: Instance | null = null;
    d.transact(`Insert ${(props.Name as string) ?? className}`, () => {
      const target = !newPart && !at && part ? part : (holder = this.makeHolderPart(d, className, at, partProps));
      inst.setParent(target);
    });
    d.selection.set([holder ?? inst]);
    return [holder ?? inst];
  }

  insertSound(name: string, soundId: string, playing: boolean): Instance[] {
    const d = this.requireEditDoc();
    if (!d) return [];
    const s = new Instance('Sound');
    Object.assign(s.props, { Name: name, SoundId: soundId, Looped: true, Playing: playing, Volume: 0.4 });
    const sel = d.selection.primary;
    const parent = sel && sel.isA('BasePart') && !sel.locked ? sel : d.dm.getService('SoundService')!;
    d.transact(`Insert ${name}`, () => s.setParent(parent));
    d.selection.set([s]);
    return [s];
  }

  addAnimation(assetId: string, name: string): Instance[] {
    const d = this.requireEditDoc();
    if (!d) return [];
    let targets = d.selection.list.filter((i) => (i.isA('BasePart') || i.className === 'Model') && !i.locked);
    const part = this.selectedPart();
    if (!targets.length && part) targets = [part];
    if (!targets.length) { this.output.warn(t('{name} animation: select a part or model first, then click the item again.', { name })); return []; }
    const made: Instance[] = [];
    d.transact(`Add ${name} animation`, () => targets.forEach((t) => {
      const k = new Instance('KeyframeAnimation');
      Object.assign(k.props, { Name: name, AnimationId: assetId });
      k.setParent(t);
      made.push(k);
    }));
    this.output.system(t('{name} animation added – it plays when you press Play.', { name }));
    return made;
  }

  /** Inserts a script template. kind: 'part' (into selected part), 'button' (into selected TextButton) or 'service'. */
  insertScriptTemplate(template: string, kind: 'part' | 'button' | 'service'): Instance[] {
    const d = this.requireEditDoc();
    if (!d) return [];
    const sel = d.selection.primary;
    let parent: Instance | undefined;
    if (kind === 'part') {
      parent = this.selectedPart();
      if (!parent) { this.output.warn(t('This script controls a part: select the part in the viewport or Explorer first, then click the script again.')); return []; }
    } else if (kind === 'button') {
      parent = sel && sel.className === 'TextButton' ? sel : undefined;
      if (!parent) { this.output.warn(t('Select a Button (TextButton) first – insert one from Toolbox > UI.')); return []; }
    } else parent = d.dm.getService('ServerScriptService')!;
    const cls = kind === 'button' ? 'LocalScript' : 'Script';
    const s = new Instance(cls);
    s.props.Name = template.replace(/\(.*\)/, '').replace(/[^\w]+/g, '') || cls;
    s.props.Source = SCRIPT_TEMPLATES[template];
    d.transact(`Insert ${template} script`, () => s.setParent(parent!));
    d.selection.set([s]);
    this.openScript(s);
    return [s];
  }

  /** A see-through trigger volume (zone) with a LocalScript, e.g. a shop or a fuel station. */
  insertZone(name: string, color: string, size: Vec3, scriptName: string, source: string, at?: Vec3): Instance | null {
    const d = this.requireEditDoc();
    if (!d) return null;
    const zone = new Instance('Part');
    Object.assign(zone.props, { Name: name, Size: size, Color: color, Transparency: 0.7, Anchored: true, CanCollide: false, Material: 'Neon', CastShadow: false });
    zone.props.Position = this.placeFor(size, at);
    const s = new Instance('LocalScript');
    s.props.Name = scriptName;
    s.props.Source = source;
    d.transact(`Insert ${name}`, () => { zone.setParent(d.dm.workspace); s.setParent(zone); });
    d.selection.set([zone]);
    this.openScript(s);
    return zone;
  }

  insertShape(shape: 'Block' | 'Ball' | 'Cylinder' | 'Wedge', at?: Vec3) {
    if (shape === 'Wedge') return this.insertObject('WedgePart', undefined, at);
    const d = this.requireEditDoc();
    if (!d) return null;
    const p = new Instance('Part');
    p.props.Shape = shape;
    p.props.Anchored = true;
    if (shape === 'Ball' || shape === 'Cylinder') p.props.Size = v3(2, 2, 2);
    p.props.Name = 'Part';
    p.props.Position = this.placeFor(p.props.Size as Vec3, at);
    const parent = this.defaultContainer(d, p);
    d.transact(`Insert ${shape}`, () => p.setParent(parent));
    d.selection.set([p]);
    return p;
  }

  insertPrefab(name: string, at?: Vec3) {
    const d = this.requireEditDoc(); if (!d) return;
    const m = buildPrefab(name);
    const parts = m.getDescendants().filter((x) => x.isA('BasePart'));
    const b = boundsOf(parts);
    const c = b.getCenter(new THREE.Vector3());
    // prefab bottoms sit at y=0; put the bottom on the target surface
    const target = at ?? this.viewport?.insertPosition(v3(0, 0, 0));
    if (target) this.placeInstances(d, [m], `Insert ${name}`, v3(target.x, target.y + (c.y - b.min.y), target.z), this.defaultContainer(d, m));
    else this.placeInstances(d, [m], `Insert ${name}`, undefined, d.dm.workspace);
  }

  /** Inserts a pre-styled GUI element (glass panel, gradient button, HUD counter). */
  insertStyledGui(style: 'glass' | 'gradient' | 'hud') {
    const d = this.requireEditDoc();
    if (!d) return;
    const presets: Record<string, [string, Record<string, unknown>]> = {
      glass: ['Frame', { Name: 'GlassPanel', Position: udim2(0.5, 0, 0.5, 0), AnchorPointX: 0.5, AnchorPointY: 0.5, Size: udim2(0, 340, 0, 210), BackgroundColor3: '#ffffff', BackgroundTransparency: 0.82, BackgroundBlur: 18, CornerRadius: 18, StrokeThickness: 1, StrokeColor: '#ffffff', StrokeTransparency: 0.6, ShadowSize: 34, ShadowTransparency: 0.55 }],
      gradient: ['TextButton', { Name: 'PlayButton', Position: udim2(0.5, 0, 0.5, 0), AnchorPointX: 0.5, AnchorPointY: 0.5, Size: udim2(0, 220, 0, 56), BackgroundColor3: '#6a5cff', GradientEnabled: true, GradientColor: '#2fd0ff', GradientRotation: 90, CornerRadius: 28, Text: t('Play'), TextColor3: '#ffffff', Font: 'SansBold', TextSize: 24, StrokeThickness: 2, StrokeColor: '#ffffff', StrokeTransparency: 0.55, ShadowSize: 22, ShadowTransparency: 0.5 }],
      hud: ['TextLabel', { Name: 'Counter', Position: udim2(0.5, 0, 0, 16), AnchorPointX: 0.5, AnchorPointY: 0, Size: udim2(0, 190, 0, 42), BackgroundColor3: '#10141c', BackgroundTransparency: 0.4, BackgroundBlur: 12, CornerRadius: 21, Text: t('Coins: 0'), TextColor3: '#ffffff', Font: 'SansBold', TextSize: 20, StrokeThickness: 1, StrokeColor: '#ffffff', StrokeTransparency: 0.8 }],
    };
    const [cls, props] = presets[style];
    const el = new Instance(cls);
    Object.assign(el.props, props);
    d.transact(t('Insert UI'), () => el.setParent(this.defaultContainer(d, el, true)));
    d.selection.set([el]);
    this.setUiEditor(true);
  }

  /** Runs a Toolbox item (click, or drop at a world position). */
  useToolboxItem(item: ToolboxItem, at?: Vec3) {
    if (!this.requireEditDoc()) return;
    try {
      item.insert(this, at);
      markToolboxUsed(item.id);
      this.emit();
    } catch (e) { this.error(t('Could not insert {name}', { name: t(item.name) }), e); }
  }

  /** Inserts or applies an asset (from the asset browser). */
  async useAsset(a: AssetRecord) {
    const d = this.editDoc;
    if (!d) { this.output.warn(t('Stop the test to use assets.')); return; }
    try {
      switch (a.kind) {
        case 'model':
          if (a.mime === 'application/x-forge-prefab') return this.insertPrefab(a.data);
          if (a.mime === 'application/x-forge-model') {
            const parsed = parseFile(a.data);
            if (parsed.format !== 'forge-model') throw new Error('Invalid model asset');
            this.insertModelFile(d, parsed);
            return;
          }
          {
            const size = await modelNaturalSize(a);
            const mp = new Instance('MeshPart');
            mp.props.Name = a.name;
            mp.props.MeshId = a.id;
            mp.props.Anchored = true;
            const sz = v3(r3(Math.max(size.x, 0.05)), r3(Math.max(size.y, 0.05)), r3(Math.max(size.z, 0.05)));
            mp.props.Size = sz;
            mp.props.Position = this.viewport?.insertPosition(sz) ?? v3(0, sz.y / 2, 0);
            d.transact(`Insert ${a.name}`, () => mp.setParent(d.dm.workspace));
            d.selection.set([mp]);
          }
          return;
        case 'material':
          return this.setPropOnSelection('Material', a.data);
        case 'texture': {
          const sel = d.selection.list;
          const guiImg = sel.filter((i) => i.className === 'ImageLabel');
          if (guiImg.length) { d.transact('Set Image', () => guiImg.forEach((i) => i.set('Image', a.id))); return; }
          return this.setPropOnSelection('TextureId', a.id);
        }
        case 'sound': {
          const sel = d.selection.primary;
          const s = new Instance('Sound');
          s.props.Name = a.name;
          s.props.SoundId = a.id;
          const parent = sel && !sel.locked ? sel : d.dm.workspace;
          d.transact(`Insert sound ${a.name}`, () => s.setParent(parent));
          d.selection.set([s]);
          return;
        }
        case 'animation': {
          const targets = d.selection.list.filter((i) => i.isA('BasePart') || i.className === 'Model');
          if (!targets.length) { this.output.warn(t('Select a part or model to animate.')); return; }
          const made: Instance[] = [];
          d.transact(`Add animation ${a.name}`, () => targets.forEach((t) => {
            const k = new Instance('KeyframeAnimation');
            k.props.Name = a.name;
            k.props.AnimationId = a.id;
            k.setParent(t);
            made.push(k);
          }));
          d.selection.set(made);
          return;
        }
        case 'script': {
          const s = new Instance('Script');
          s.props.Name = a.name.replace(/[^\w]+/g, '') || 'Script';
          s.props.Source = a.data;
          const sel = d.selection.primary;
          const parent = sel && !sel.locked ? sel : d.dm.getService('ServerScriptService')!;
          d.transact(`Insert script ${a.name}`, () => s.setParent(parent));
          d.selection.set([s]);
          this.openScript(s);
          return;
        }
      }
    } catch (e) { this.error(t('Could not use asset "{name}"', { name: a.name }), e); }
  }

  async importAssets(): Promise<AssetRecord[]> {
    const d = this.active; if (!d) return [];
    const out: AssetRecord[] = [];
    try {
      const files = await openFiles('asset', true);
      for (const f of files) {
        try {
          const rec = d.assets.importFile(f.name, f.data);
          if (rec.kind === 'model' && rec.mime !== 'application/x-forge-model') await modelNaturalSize(rec); // validate glTF now
          if (rec.mime === 'application/x-forge-model') { const p = parseFile(rec.data); if (p.format !== 'forge-model') throw new Error('Not a model file'); }
          out.push(rec);
          this.output.system(t('Imported {kind} "{name}" into the Assets panel.', { kind: rec.kind, name: rec.name }));
        } catch (e) {
          this.error(t('Import of {name} failed', { name: f.name }), e);
          const bad = d.assets.list().find((x) => x.name === f.name.replace(/\.[^.]+$/, '') && !out.includes(x));
          if (bad) d.assets.remove(bad.id);
        }
      }
    } catch (e) { this.error(t('Import failed'), e); }
    return out;
  }

  async exportAsset(a: AssetRecord) {
    const ext = EXT_BY_MIME[a.mime] ?? (a.mime === 'application/x-forge-prefab' ? 'forgemodel' : 'bin');
    try {
      let data: string | ArrayBuffer = a.encoding === 'text' ? a.data : assetBytes(a);
      if (a.mime === 'application/x-forge-prefab') {
        const m = buildPrefab(a.data);
        data = JSON.stringify({ format: 'forge-model', version: FORMAT_VERSION, name: a.name, instances: [serializeInstance(m, false)], assets: [] });
      }
      const kind: FileKind = ext === 'lua' ? 'lua' : ext === 'forgemodel' ? 'model' : ext === 'glb' ? 'glb' : 'asset';
      const res = await saveFile(kind, `${a.name}.${ext}`, data);
      if (res) this.output.system(t('Exported asset "{name}" to "{file}".', { name: a.name, file: res.name }));
    } catch (e) { this.error(t('Asset export failed'), e); }
  }

  /** Saves the selection as a reusable model asset in the project library. */
  saveSelectionAsAsset() {
    const d = this.editDoc;
    if (!d || !d.selection.size) { this.output.warn(t('Select objects to save to the asset library.')); return; }
    const list = this.topLevel(this.editable(d.selection.list));
    const name = list.length === 1 ? list[0].name : 'Model';
    const file = this.makeModelFile(d, list, name);
    d.assets.add({ name, kind: 'model', mime: 'application/x-forge-model', data: JSON.stringify(file), encoding: 'text' });
    this.output.system(t('Saved "{name}" to the project\'s asset library.', { name }));
    this.panelRequest.fire('assets');
  }

  // ---- Scripts -------------------------------------------------------------------

  openScript(inst: Instance) {
    const d = this.active;
    if (!d || !inst.isA('LuaSourceContainer')) return;
    if (this.play) { this.output.warn(t('Scripts open in the editor show the edit version; changes apply after stopping the test.')); }
    d.openScript(inst);
  }

  async importLuaScript() {
    const d = this.editDoc; if (!d) return;
    try {
      const files = await openFiles('lua', true);
      for (const f of files) {
        const s = new Instance('Script');
        s.props.Name = f.name.replace(/\.[^.]+$/, '').replace(/[^\w]+/g, '') || 'Script';
        s.props.Source = new TextDecoder().decode(f.data);
        const parent = d.selection.primary && !d.selection.primary.locked ? d.selection.primary : d.dm.getService('ServerScriptService')!;
        d.transact('Import script', () => s.setParent(parent));
        this.openScript(s);
      }
    } catch (e) { this.error(t('Script import failed'), e); }
  }

  async exportLuaScript(inst?: Instance) {
    const d = this.active;
    const s = inst ?? (d && d.activeTab !== 'scene' ? d.dm.byId(d.activeTab) : d?.selection.primary);
    if (!s || !s.isA('LuaSourceContainer')) { this.output.warn(t('Select or open a script to export.')); return; }
    try {
      const res = await saveFile('lua', `${s.name}.lua`, s.props.Source as string);
      if (res) this.output.system(t('Saved script "{name}" to "{file}".', { name: s.name, file: res.name }));
    } catch (e) { this.error(t('Script export failed'), e); }
  }

  // ---- Command bar & diagnostics ---------------------------------------------------

  private editRt: { doc: Document; rt: Promise<LuaRuntime>; off: () => void; time: number } | null = null;

  /** Runs Lua from the command bar: inside the running test, or on the edit scene (undoable). */
  async runCommand(code: string) {
    this.output.log('info', `> ${code}`);
    if (this.play) { this.play.exec(code); return; }
    const d = this.active;
    if (!d) { this.output.warn(t('Open a project first.')); return; }
    if (!this.editRt || this.editRt.doc !== d || !this.docs.includes(this.editRt.doc)) {
      this.disposeEditRuntime();
      const rt = LuaRuntime.create(d.dm, {
        log: (l, m, _s, line) => this.output.log(l === 'print' ? 'print' : l, line ? `${m}` : m),
        mutate: (fn) => d.transact('Command Bar', fn),
      });
      const entry = { doc: d, rt, off: () => {}, time: 0 };
      const vp = this.viewport;
      if (vp) entry.off = vp.beforeRender.connect((dt) => { if (this.play) return; entry.time += dt; rt.then((r) => r.step(dt, entry.time)); });
      this.editRt = entry;
    }
    try { (await this.editRt.rt).exec(code); } catch (e) { this.output.error((e as Error).message); }
  }

  private disposeEditRuntime() {
    if (!this.editRt) return;
    this.editRt.off();
    this.editRt.rt.then((r) => r.dispose()).catch(() => {});
    this.editRt = null;
  }

  async runSelfTest() {
    if (this.play) return;
    this.output.system(t('Running self-test…'));
    this.panelRequest.fire('output');
    const results = await runSelfTest((r) => this.output.log(r.ok ? 'info' : 'error', `${r.ok ? 'PASS' : 'FAIL'}  ${r.name} – ${r.detail}`));
    const failed = results.filter((r) => !r.ok).length;
    this.output.log(failed ? 'error' : 'system', t('Self-test finished: {ok}/{n} passed.', { ok: results.length - failed, n: results.length }));
    return results;
  }

  // ---- Publishing (git-like versions on a Forge Server) ---------------------------------

  localServer: ServerInfo | null = null;

  /** Publish target of the active project (for the status bar). */
  get published() { return this.active?.settings.publish ?? null; }

  /** Remembered publish key per server URL (local storage, never inside the project file). */
  publishKeyFor(server: string): string {
    try { return (JSON.parse(localStorage.getItem('forge.publishKeys') ?? '{}') as Record<string, string>)[normalizeServerUrl(server)] ?? ''; } catch { return ''; }
  }
  rememberPublishKey(server: string, key: string) {
    try {
      const m = JSON.parse(localStorage.getItem('forge.publishKeys') ?? '{}') as Record<string, string>;
      m[normalizeServerUrl(server)] = key;
      localStorage.setItem('forge.publishKeys', JSON.stringify(m));
    } catch { /* storage unavailable */ }
  }

  async toggleLocalServer(): Promise<ServerInfo | null> {
    if (!native) return null;
    const running = (await native.serverStatus()).running;
    const info = running ? await native.serverStop() : await native.serverStart(8787);
    this.localServer = info;
    if (info.error) this.output.error(t('Local server: {error}', { error: info.error }));
    else if (info.running && info.url) {
      if (info.publishKey) this.rememberPublishKey(info.url, info.publishKey);
      this.output.system(t('Local Forge Server running at {url}. Players on this network can join via Forge Player.', { url: info.url }));
    } else this.output.system(t('Local Forge Server stopped.'));
    this.emit();
    return info;
  }

  async refreshLocalServer() { if (native) { this.localServer = await native.serverStatus(); this.emit(); } }

  /**
   * Publishes the active project as a new immutable version. `base` is the
   * version this project was based on; the server rejects the publish if
   * someone published in between (unless `force`).
   */
  async publish(opts: { server: string; key: string; gameId: string | null; newGame?: { name: string; description: string }; message: string; author: string; force?: boolean }): Promise<VersionInfo> {
    const doc = this.active;
    if (!doc) throw new Error(t('Open a project first.'));
    if (this.play) throw new Error(t('Stop the test before publishing.'));
    const api = new ServerApi(opts.server, opts.key);
    let gameId = opts.gameId;
    if (!gameId) {
      const g = await api.createGame(opts.newGame?.name || doc.name, opts.newGame?.description ?? '', opts.author);
      gameId = g.id;
    }
    const sameTarget = doc.settings.publish && doc.settings.publish.gameId === gameId && normalizeServerUrl(doc.settings.publish.server) === api.base;
    const parent = sameTarget ? doc.settings.publish!.version : (await api.game(gameId)).history.at(-1)?.number ?? null;
    if (this.viewport && this.active === doc) this.viewport.saveCameraTo(doc);
    const file = doc.toProjectFile();
    delete file.settings.publish; // the published copy does not need the publish target
    const v = await api.publish(gameId, {
      parent, message: opts.message, author: opts.author, project: JSON.stringify(file),
      thumbnail: this.viewport?.captureThumbnail(480, 270) ?? null, live: true, force: !!opts.force,
    });
    this.rememberPublishKey(api.base, opts.key);
    doc.settings.publish = { server: api.base, gameId, version: v.number };
    doc.history.touch();
    this.output.system(t('Published "{name}" as version {v} – it is live now. Players can join it in Forge Player.', { name: doc.name, v: v.number }));
    this.emit();
    return v;
  }

  /** Opens a published version as a new project tab ("checkout"). */
  async checkoutVersion(server: string, gameId: string, version: number) {
    const api = new ServerApi(server);
    const { text } = await api.project(gameId, version);
    const doc = this.loadProjectData(`v${version}`, text, null);
    if (doc) {
      doc.name = `${doc.name} (v${version})`;
      doc.settings.publish = { server: api.base, gameId, version };
      doc.history.touch();
      this.emit();
    }
  }

  // ---- Media import ------------------------------------------------------------------

  /** Imports an image / sound / glTF file and puts it into the scene in a sensible way. */
  async importMedia(kind: 'image' | 'sound' | 'glb', files?: { name: string; data: ArrayBuffer }[], at?: Vec3) {
    const d = this.requireEditDoc();
    if (!d) return;
    try {
      const list = files ?? await openFiles(kind, true);
      for (const f of list) {
        const rec = d.assets.importFile(f.name, f.data);
        if (rec.kind === 'texture') this.insertImage(d, rec, at);
        else if (rec.kind === 'sound') this.insertSoundAsset(d, rec);
        else if (rec.kind === 'model') {
          if (rec.mime === 'application/x-forge-model') { const p = parseFile(rec.data); if (p.format === 'forge-model') this.insertModelFile(d, p, at); }
          else await this.insertMeshAsset(d, rec, at);
        } else this.output.system(t('Imported {kind} "{name}" into the Assets panel.', { kind: rec.kind, name: rec.name }));
      }
    } catch (e) { this.error(t('Import failed'), e); }
  }

  /** Files dragged from the operating system into the 3D view or the Assets panel. */
  async dropFiles(files: File[], at?: Vec3) {
    const d = this.requireEditDoc();
    if (!d) return;
    for (const f of files) {
      const ext = f.name.split('.').pop()!.toLowerCase();
      const data = await f.arrayBuffer();
      const kind = ['png', 'jpg', 'jpeg', 'webp'].includes(ext) ? 'image' : ['mp3', 'wav', 'ogg'].includes(ext) ? 'sound' : ['glb', 'gltf', 'forgemodel'].includes(ext) ? 'glb' : null;
      if (kind) { await this.importMedia(kind, [{ name: f.name, data }], at); continue; }
      if (ext === 'forge' || ext === 'forgescene') { try { this.loadProjectData(f.name, new TextDecoder().decode(data), null); } catch (e) { this.error(t('Could not open "{name}"', { name: f.name }), e); } continue; }
      try { const rec = d.assets.importFile(f.name, data); this.output.system(t('Imported {kind} "{name}" into the Assets panel.', { kind: rec.kind, name: rec.name })); }
      catch (e) { this.error(t('Import failed'), e); }
    }
  }

  /** Image: a Decal on the selected part, or a new thin picture panel at the view center. */
  private insertImage(d: Document, rec: AssetRecord, at?: Vec3) {
    const part = at ? undefined : this.selectedPart();
    const decal = new Instance('Decal');
    decal.props.Name = rec.name;
    decal.props.Texture = rec.id;
    let created: Instance = decal;
    d.transact(t('Insert image'), () => {
      let target = part;
      if (!target) {
        const panel = new Instance('Part');
        Object.assign(panel.props, { Name: rec.name, Size: v3(6, 6, 0.2), Anchored: true, Color: '#ffffff', Material: 'SmoothPlastic' });
        panel.props.Position = this.placeFor(panel.props.Size as Vec3, at);
        // turn the picture (Decal Face = Front, -Z) toward the camera
        const cam = this.viewport?.camera.position, pos = panel.props.Position as Vec3;
        if (cam) panel.props.Orientation = v3(0, Math.round(Math.atan2(-(cam.x - pos.x), -(cam.z - pos.z)) * 180 / Math.PI / 15) * 15, 0);
        panel.setParent(d.dm.workspace);
        target = panel;
        created = panel;
      }
      else decal.props.Face = this.viewport?.faceTowardCamera(target) ?? 'Front';
      decal.setParent(target);
    });
    d.selection.set([created]);
    this.output.system(t('Inserted image "{name}".', { name: rec.name }));
  }

  private insertSoundAsset(d: Document, rec: AssetRecord) {
    const part = this.selectedPart();
    const s = new Instance('Sound');
    Object.assign(s.props, { Name: rec.name, SoundId: rec.id });
    const parent = part ?? d.dm.getService('SoundService')!;
    d.transact(t('Insert sound'), () => s.setParent(parent));
    d.selection.set([s]);
    this.output.system(part ? t('Added sound "{name}" to {part} (3D sound).', { name: rec.name, part: part.name }) : t('Added sound "{name}" to SoundService. Set Playing or call :Play() from a script.', { name: rec.name }));
  }

  private async insertMeshAsset(d: Document, rec: AssetRecord, at?: Vec3) {
    const size = await modelNaturalSize(rec);
    const mp = new Instance('MeshPart');
    const sz = v3(r3(Math.max(size.x, 0.05)), r3(Math.max(size.y, 0.05)), r3(Math.max(size.z, 0.05)));
    Object.assign(mp.props, { Name: rec.name, MeshId: rec.id, Anchored: true, Size: sz });
    mp.props.Position = this.placeFor(sz, at);
    d.transact(t('Insert model'), () => mp.setParent(d.dm.workspace));
    d.selection.set([mp]);
  }

  // ---- UI editor ------------------------------------------------------------------------

  uiEditor = false;
  setUiEditor(on: boolean) {
    this.uiEditor = on;
    this.viewport?.gui.setEditing(on && !this.play ? this.active : null);
    if (on) this.output.system(t('UI editor on: click elements on screen to select them, drag to move, drag the corner handles to resize.'));
    this.emit();
  }

  // ---- Play ------------------------------------------------------------------------

  async startPlay() {
    if (this.play || !this.active || !this.viewport) return;
    const doc = this.active;
    this.viewport.saveCameraTo(doc);
    doc.selection.clear();
    try {
      const session = Studio.createPlaySession(this, doc);
      this.play = session;
      this.emit();
      await session.start();
    } catch (e) {
      this.error(t('Could not start the test'), e);
      await this.stopPlay();
    }
    this.emit();
  }

  async stopPlay() {
    const s = this.play;
    if (!s) return;
    s.stop();
    this.play = null;
    this.bindViewport();
    this.viewport?.setTool(this.tool);
    this.emit();
  }

  togglePause() { if (this.play) { this.play.paused ? this.play.resume() : this.play.pause(); this.emit(); } }

  async restartPlay() { await this.stopPlay(); await this.startPlay(); }

  // ---- Context menus ------------------------------------------------------------------

  showContextMenu(x: number, y: number, target: Instance | null, origin: 'viewport' | 'explorer') {
    const d = this.active;
    if (!d) return;
    const sel = d.selection;
    const has = sel.size > 0;
    const anyLocked = sel.list.some((i) => i.locked);
    const playing = !!this.play;
    const items: ContextMenuItem[] = [];
    if (target?.isA('LuaSourceContainer')) items.push({ label: t('Open Script'), action: () => this.openScript(target) }, { separator: true });
    items.push(
      { label: t('Cut'), shortcut: 'Ctrl+X', action: () => this.cut(), disabled: !has || anyLocked || playing },
      { label: t('Copy'), shortcut: 'Ctrl+C', action: () => this.copy(), disabled: !has },
      { label: t('Paste'), shortcut: 'Ctrl+V', action: () => this.paste(), disabled: !this.clipboard || playing },
      { label: t('Paste Into'), shortcut: 'Ctrl+Shift+V', action: () => this.paste(true), disabled: !this.clipboard || !has || playing },
      { label: t('Duplicate'), shortcut: 'Ctrl+D', action: () => this.duplicate(), disabled: !has || anyLocked || playing },
      { label: t('Delete'), shortcut: 'Del', action: () => this.deleteSelection(), disabled: !has || anyLocked || playing },
      { label: t('Rename'), shortcut: 'F2', action: () => this.rename(), disabled: !has || anyLocked || playing },
      { separator: true },
      { label: t('Group as Model'), shortcut: 'Ctrl+G', action: () => this.group('Model'), disabled: !has || anyLocked || playing },
      { label: t('Group as Folder'), shortcut: 'Ctrl+Alt+G', action: () => this.group('Folder'), disabled: !has || anyLocked || playing },
      { label: t('Ungroup'), shortcut: 'Ctrl+U', action: () => this.ungroup(), disabled: !has || playing },
      { label: t('Select Children'), action: () => this.selectChildren(), disabled: !has },
      { label: t('Zoom To'), shortcut: 'F', action: () => this.focusSelection(), disabled: !has || !collectTransformables(sel.list).length },
      { separator: true },
      {
        label: t('Insert Object'), disabled: playing, children: insertMenuItems(this, target ?? undefined),
      },
      { label: t('Toggle Anchored'), action: () => this.toggleProp('Anchored'), disabled: !has || playing },
      { label: t('Toggle Lock'), action: () => this.toggleProp('Locked'), disabled: !has || playing },
      { separator: true },
      { label: t('Save to Asset Library'), action: () => this.saveSelectionAsAsset(), disabled: !has || anyLocked || playing },
      { label: t('Export Selection…'), action: () => this.exportSelection(), disabled: !has || anyLocked },
    );
    if (origin === 'explorer' && target && target.children.length) {
      items.push({ separator: true }, { label: t('Expand All'), action: () => this.panelRequest.fire('explorer:expand:' + target.id) }, { label: t('Collapse All'), action: () => this.panelRequest.fire('explorer:collapse:' + target.id) });
    }
    this.contextMenu.fire({ x, y, items });
  }

  // ---- Shortcuts -----------------------------------------------------------------------

  handleKey(e: KeyboardEvent): boolean {
    const target = e.target as HTMLElement | null;
    const inText = !!target && (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA' || target.isContentEditable || !!target.closest?.('.monaco-editor'));
    const ctrl = e.ctrlKey || e.metaKey;
    const k = e.key.toLowerCase();
    const run = (fn: () => void) => { e.preventDefault(); fn(); return true; };
    // global even inside text fields
    if (ctrl && k === 's') return run(() => this.save(this.active, e.shiftKey));
    if (ctrl && k === 'o') return run(() => this.openProjectDialog());
    if (ctrl && k === 'n') return run(() => this.newProject());
    if (e.key === 'F8' || (e.shiftKey && e.key === 'F5')) return run(() => this.stopPlay());
    if (e.key === 'F5') return run(() => (this.play ? this.togglePause() : this.startPlay()));
    if (inText) return false;
    if (ctrl && k === 'z') return run(() => (e.shiftKey ? this.redo() : this.undo()));
    if (ctrl && k === 'y') return run(() => this.redo());
    if (ctrl && k === 'c') return run(() => this.copy());
    if (ctrl && k === 'x') return run(() => this.cut());
    if (ctrl && k === 'v') return run(() => this.paste(e.shiftKey));
    if (ctrl && k === 'd') return run(() => this.duplicate());
    if (ctrl && k === 'a') return run(() => this.selectAll());
    if (ctrl && k === 'g') return run(() => this.group(e.altKey ? 'Folder' : 'Model'));
    if (ctrl && k === 'u') return run(() => this.ungroup());
    if (ctrl && k === 'l') return run(() => { this.viewport?.setSpace(this.viewport.space === 'world' ? 'local' : 'world'); this.output.system(this.viewport?.space === 'local' ? t('Local space') : t('World space')); this.emit(); });
    if (ctrl && e.key === '1') return run(() => this.setTool('select'));
    if (ctrl && e.key === '2') return run(() => this.setTool('move'));
    if (ctrl && e.key === '3') return run(() => this.setTool('scale'));
    if (ctrl && e.key === '4') return run(() => this.setTool('rotate'));
    if (e.key === 'Delete' || e.key === 'Backspace') return run(() => this.deleteSelection());
    if (e.key === 'F2') return run(() => this.rename());
    if (!ctrl && !e.altKey && k === 'f' && this.active && !this.play) return run(() => this.focusSelection());
    if (e.key === 'Escape' && this.active && !this.play) return run(() => this.active!.selection.clear());
    return false;
  }

  /** Called by the window close handler (Electron) – asks about unsaved documents. */
  async requestQuit(): Promise<boolean> {
    for (const d of [...this.docs]) if (!(await this.closeDoc(d))) return false;
    return true;
  }
}

function r3(x: number) { return Math.round(x * 1000) / 1000; }

export const INSERT_GROUPS = ['World', 'Parts', 'Containers', 'Scripts', 'Lights', 'Effects', 'Interaction', 'Environment', 'GUI', 'Values'];

export function insertMenuItems(studio: Studio, parent?: Instance): ContextMenuItem[] {
  return INSERT_GROUPS.map((g) => ({
    label: t(g),
    children: creatableClasses().filter((c) => c.insertCategory === g).map((c) => ({ label: c.name, action: () => studio.insertObject(c.name, parent) })),
  }));
}

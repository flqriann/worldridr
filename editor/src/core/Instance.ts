// The extensible object system. Every object in a scene is an Instance with
// a class name, a property bag (schema in classes.ts), children and a parent.
// Instances attached to a DataModel emit change events (consumed by the
// viewport, UI, physics and script runtime) and record undo history.

import { CLASS_DEFAULT_OVERRIDES, getClass, getPropDef, getProps, isA as classIsA } from './classes';
import { Signal } from './Signal';
import { cloneValue, newId, valueEqual } from './types';
import type { History } from './History';
import type { TerrainData } from '../terrain/TerrainData';

export class Instance {
  readonly id: string;
  readonly className: string;
  parent: Instance | null = null;
  readonly children: Instance[] = [];
  readonly props: Record<string, unknown> = {};
  attributes: Record<string, unknown> = {};
  /** The DataModel this instance is attached to (null when detached). */
  dm: DataModel | null = null;
  /** Heightmap data for the Terrain class. */
  terrain?: TerrainData;
  /** Transient flag set by the runtime for objects it created (characters etc.). */
  runtimeOnly = false;

  constructor(className: string, id?: string) {
    const def = getClass(className);
    if (!def) throw new Error(`Unknown class "${className}"`);
    if (def.abstract) throw new Error(`Cannot create abstract class "${className}"`);
    this.className = className;
    this.id = id ?? newId();
    for (const pd of getProps(className)) this.props[pd.name] = cloneValue(pd.default);
    const ov = CLASS_DEFAULT_OVERRIDES[className];
    if (ov) for (const k of Object.keys(ov)) this.props[k] = cloneValue(ov[k]);
    this.props.Name = className;
    this.props.ClassName = className;
  }

  get name(): string { return this.props.Name as string; }
  set name(v: string) { this.set('Name', v); }
  get locked(): boolean { return !!getClass(this.className)?.locked; }

  isA(base: string) { return classIsA(this.className, base); }

  get<T = unknown>(prop: string): T { return this.props[prop] as T; }

  /** Sets a property, emitting a change event and recording undo history. */
  set(prop: string, value: unknown): void {
    const def = getPropDef(this.className, prop);
    if (!def) throw new Error(`${prop} is not a valid property of ${this.className}`);
    if (def.readOnly && prop !== 'ClassName') throw new Error(`${prop} is read-only`);
    if (prop === 'ClassName') return;
    const old = this.props[prop];
    if (valueEqual(old, value)) return;
    this.props[prop] = value;
    const dm = this.dm;
    if (dm) {
      dm.history?.record({ k: 'prop', inst: this, prop, before: old, after: value });
      dm.changed.fire(this, prop, value, old);
    }
  }

  /** Set without history (used by runtime physics sync and undo itself). */
  setSilent(prop: string, value: unknown): void {
    const old = this.props[prop];
    if (valueEqual(old, value)) return;
    this.props[prop] = value;
    this.dm?.changed.fire(this, prop, value, old);
  }

  setAttribute(key: string, value: unknown) {
    const before = this.attributes[key];
    if (valueEqual(before, value)) return;
    if (value === undefined || value === null) delete this.attributes[key];
    else this.attributes[key] = value;
    this.dm?.history?.record({ k: 'attr', inst: this, key, before, after: value });
    this.dm?.attributeChanged.fire(this, key);
  }

  // ---- Hierarchy -------------------------------------------------------

  isDescendantOf(other: Instance): boolean {
    let p = this.parent;
    while (p) { if (p === other) return true; p = p.parent; }
    return false;
  }

  canParentTo(newParent: Instance | null): string | null {
    if (newParent === this) return 'Cannot parent an object to itself';
    if (newParent && newParent.isDescendantOf(this)) return 'Cannot parent an object to one of its descendants';
    if (this.locked && this.parent && newParent !== this.parent) return `${this.className} cannot be moved`;
    return null;
  }

  setParent(newParent: Instance | null, index?: number): void {
    const err = this.canParentTo(newParent);
    if (err) throw new Error(err);
    const oldParent = this.parent;
    const oldIndex = oldParent ? oldParent.children.indexOf(this) : -1;
    if (oldParent === newParent && (index === undefined || index === oldIndex)) return;

    if (oldParent) oldParent.children.splice(oldIndex, 1);
    this.parent = newParent;
    let newIndex = -1;
    if (newParent) {
      const max = newParent.children.length;
      newIndex = index === undefined || index < 0 || index > max ? max : index;
      newParent.children.splice(newIndex, 0, this);
    }

    const oldDm = this.dm;
    const newDm = newParent ? (newParent instanceof DataModel ? newParent : newParent.dm) : null;
    const historyDm = oldDm ?? newDm;
    historyDm?.history?.record({ k: 'parent', inst: this, beforeParent: oldParent, beforeIndex: oldIndex, afterParent: newParent, afterIndex: newIndex });

    if (oldDm && oldDm !== newDm) {
      oldDm.detachSubtree(this, oldParent);
    }
    if (newDm && oldDm !== newDm) {
      newDm.attachSubtree(this);
    } else if (newDm && oldDm === newDm) {
      newDm.reparented.fire(this, oldParent);
    }
    oldParent?.dm?.childRemoved.fire(oldParent, this);
    newParent?.dm?.childAdded.fire(newParent, this);
  }

  destroy() { if (!this.locked) this.setParent(null); }

  findFirstChild(name: string, recursive = false): Instance | undefined {
    for (const c of this.children) if (c.name === name) return c;
    if (recursive) for (const c of this.children) { const f = c.findFirstChild(name, true); if (f) return f; }
    return undefined;
  }
  findFirstChildOfClass(className: string): Instance | undefined {
    return this.children.find((c) => c.className === className);
  }
  findFirstChildWhichIsA(className: string): Instance | undefined {
    return this.children.find((c) => c.isA(className));
  }
  findFirstAncestorOfClass(className: string): Instance | undefined {
    let p = this.parent;
    while (p) { if (p.className === className) return p; p = p.parent; }
    return undefined;
  }

  getDescendants(out: Instance[] = []): Instance[] {
    for (const c of this.children) { out.push(c); c.getDescendants(out); }
    return out;
  }

  /** Visits this instance and all descendants (pre-order). */
  walk(fn: (i: Instance) => void | false) {
    if (fn(this) === false) return;
    for (const c of [...this.children]) c.walk(fn);
  }

  getFullName(): string {
    const parts: string[] = [];
    let p: Instance | null = this;
    while (p && !(p instanceof DataModel)) { parts.unshift(p.name); p = p.parent; }
    return parts.join('.');
  }

  /** Deep copy (detached). Ids are regenerated. */
  clone(): Instance {
    const c = new Instance(this.className);
    for (const k of Object.keys(this.props)) c.props[k] = cloneValue(this.props[k]);
    c.attributes = { ...this.attributes };
    if (this.terrain) c.terrain = this.terrain.clone();
    for (const ch of this.children) {
      if (ch.runtimeOnly) continue;
      const cc = ch.clone();
      cc.parent = c;
      c.children.push(cc);
    }
    return c;
  }
}

export type HistoryOp =
  | { k: 'prop'; inst: Instance; prop: string; before: unknown; after: unknown }
  | { k: 'attr'; inst: Instance; key: string; before: unknown; after: unknown }
  | { k: 'parent'; inst: Instance; beforeParent: Instance | null; beforeIndex: number; afterParent: Instance | null; afterIndex: number }
  | { k: 'terrain'; inst: Instance; before: unknown; after: unknown };

/** Root of a scene. Owns the id index and the change signals. */
export class DataModel extends Instance {
  history: History | null = null;
  readonly index = new Map<string, Instance>();

  readonly added = new Signal<[Instance]>();
  readonly removed = new Signal<[Instance, Instance | null]>();
  readonly reparented = new Signal<[Instance, Instance | null]>();
  readonly changed = new Signal<[Instance, string, unknown, unknown]>();
  readonly attributeChanged = new Signal<[Instance, string]>();
  readonly childAdded = new Signal<[Instance, Instance]>();
  readonly childRemoved = new Signal<[Instance, Instance]>();
  /**
   * Batched transform updates from the physics simulation (Position/Orientation
   * written directly, no per-property `changed` events – like Roblox, physics
   * movement does not fire Changed).
   */
  readonly transformsChanged = new Signal<[Instance[]]>();
  /** Fired after terrain heights/materials change (region in vertex coords or null = all). */
  readonly terrainChanged = new Signal<[Instance, { x0: number; z0: number; x1: number; z1: number } | null]>();

  constructor(id?: string) {
    super('DataModel', id);
    this.props.Name = 'Game';
    this.dm = this;
    this.index.set(this.id, this);
  }

  attachSubtree(root: Instance) {
    root.walk((i) => { i.dm = this; this.index.set(i.id, i); });
    root.walk((i) => { this.added.fire(i); });
  }

  detachSubtree(root: Instance, oldParent: Instance | null) {
    const list: Instance[] = [];
    root.walk((i) => { list.push(i); });
    for (const i of list.reverse()) this.removed.fire(i, i === root ? oldParent : i.parent);
    for (const i of list) { i.dm = null; this.index.delete(i.id); }
  }

  getService(className: string): Instance | undefined { return this.findFirstChildOfClass(className); }
  get workspace(): Instance { return this.getService('Workspace')!; }
  get terrainInstance(): Instance | undefined { return this.workspace.findFirstChildOfClass('Terrain'); }

  byId(id: string) { return this.index.get(id); }
}

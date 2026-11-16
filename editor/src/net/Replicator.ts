// World replication between the host (runs server scripts + physics) and the
// other clients. The host batches changes of the replicated containers and
// sends them 20 times per second; clients apply them without echoing.
//
// Ops:  ['c', parentId, InstanceJSON, index]   create subtree (ids preserved)
//       ['d', id]                              destroy
//       ['m', id, parentId]                    re-parent
//       ['p', id, prop, value]                 property
//       ['a', id, key, value]                  attribute
//       ['x', [[id, px,py,pz, ox,oy,oz], ...]] physics transforms

import type { DataModel, Instance } from '../core/Instance';
import { LoadReport, deserializeInstance, sanitizeValue, serializeInstance, type InstanceJSON } from '../core/Serializer';
import { v3, type Vec3 } from '../core/types';

export type RepOp =
  | ['c', string, InstanceJSON, number]
  | ['d', string]
  | ['m', string, string]
  | ['p', string, string, unknown]
  | ['a', string, string, unknown]
  | ['x', (string | number)[][]];

const REPLICATED_SERVICES = ['Workspace', 'Lighting', 'ReplicatedStorage', 'SoundService', 'Players'];
/** Properties of character humanoids that cross the owner boundary. */
const HUMANOID_PROPS = new Set(['Health', 'MaxHealth', 'WalkSpeed', 'JumpPower']);

export function isCharacterInstance(i: Instance): boolean { return i.id.startsWith('char-'); }
/** Local-only containers inside a Player (GUI and scripts are per client). */
function isLocalOnly(i: Instance): boolean {
  let p: Instance | null = i;
  while (p) {
    if (p.parent?.className === 'Player' && (p.className === 'PlayerGui' || p.name === 'PlayerScripts')) return true;
    p = p.parent;
  }
  return false;
}

export class Replicator {
  private created = new Set<Instance>();
  private ops: RepOp[] = [];
  private props = new Map<string, RepOp>();
  private transforms = new Map<string, (string | number)[]>();
  private unsub: (() => void)[] = [];
  /** True while applying remote ops (prevents echo). */
  applying = false;
  private roots: Instance[];

  constructor(private dm: DataModel) {
    this.roots = REPLICATED_SERVICES.map((s) => dm.getService(s)).filter((s): s is Instance => !!s);
  }

  isReplicated(i: Instance | null): boolean {
    if (!i || !i.dm) return false;
    if (isLocalOnly(i)) return false;
    return this.roots.some((r) => i === r || i.isDescendantOf(r));
  }

  private inCreated(i: Instance) {
    let p: Instance | null = i;
    while (p) { if (this.created.has(p)) return true; p = p.parent; }
    return false;
  }

  // ---- host side --------------------------------------------------------------------

  /** Starts collecting changes (host). */
  startHost() {
    const dm = this.dm;
    this.unsub.push(
      dm.added.connect((i) => {
        if (this.applying || isCharacterInstance(i) || !this.isReplicated(i)) return;
        if (i.parent && this.inCreated(i.parent)) return;
        this.created.add(i);
      }),
      dm.removed.connect((i, oldParent) => {
        // only the root of a destroyed subtree (its parent is already cleared)
        if (this.applying || isCharacterInstance(i) || i.parent !== null || !this.isReplicated(oldParent)) return;
        if (this.created.delete(i)) return;
        this.ops.push(['d', i.id]);
      }),
      dm.reparented.connect((i, oldParent) => {
        if (this.applying || isCharacterInstance(i)) return;
        const was = this.isReplicated(oldParent), now = this.isReplicated(i);
        if (now && !was) { if (!this.inCreated(i)) this.created.add(i); }
        else if (was && !now) { if (!this.created.delete(i)) this.ops.push(['d', i.id]); }
        else if (now && was && !this.inCreated(i)) this.ops.push(['m', i.id, i.parent!.id]);
      }),
      dm.changed.connect((i, prop, value) => {
        if (this.applying || prop === 'ClassName') return;
        if (isCharacterInstance(i)) {
          if (i.className !== 'Humanoid' || !HUMANOID_PROPS.has(prop)) return;
        } else if (!this.isReplicated(i) || this.inCreated(i)) return;
        if (prop === 'Position' || prop === 'Orientation') this.transforms.delete(i.id);
        this.props.set(i.id + '\u0000' + prop, ['p', i.id, prop, value]);
      }),
      dm.attributeChanged.connect((i, key) => {
        if (this.applying || !this.isReplicated(i) || this.inCreated(i)) return;
        this.ops.push(['a', i.id, key, i.attributes[key] ?? null]);
      }),
      dm.transformsChanged.connect((list) => {
        if (this.applying) return;
        for (const i of list) {
          if (isCharacterInstance(i) || this.inCreated(i)) continue;
          const p = i.props.Position as Vec3, o = i.props.Orientation as Vec3;
          this.transforms.set(i.id, [i.id, p.x, p.y, p.z, o.x, o.y, o.z]);
        }
      }),
    );
  }

  /** Collects the pending ops (call ~20x per second on the host). */
  flush(): RepOp[] {
    const out: RepOp[] = [];
    for (const root of this.created) {
      if (!root.dm || !root.parent) continue;
      out.push(['c', root.parent.id, serializeInstance(root, true), root.parent.children.indexOf(root)]);
    }
    this.created.clear();
    out.push(...this.ops);
    this.ops = [];
    out.push(...this.props.values());
    this.props.clear();
    if (this.transforms.size) { out.push(['x', [...this.transforms.values()]]); this.transforms.clear(); }
    return out;
  }

  /** Full snapshot of the replicated containers for a joining player. */
  snapshot(): { parent: string; json: InstanceJSON }[] {
    const out: { parent: string; json: InstanceJSON }[] = [];
    for (const root of this.roots) {
      for (const c of root.children) {
        if (isCharacterInstance(c) || c.className === 'Terrain' || c.className === 'Camera') continue;
        if (root.className === 'Players') {
          // per-player server-created children (e.g. leaderstats)
          for (const pc of c.children) if (!isLocalOnly(pc)) out.push({ parent: c.id, json: serializeInstance(pc, true) });
          continue;
        }
        out.push({ parent: root.id, json: serializeInstance(c, true) });
      }
    }
    return out;
  }

  // ---- client side ------------------------------------------------------------------

  /** Applies host ops. Returns instances whose transforms changed (for physics). */
  apply(ops: RepOp[]): Instance[] {
    const moved: Instance[] = [];
    const report = new LoadReport();
    this.applying = true;
    try {
      for (const op of ops) {
        try {
          switch (op[0]) {
            case 'c': {
              const parent = this.dm.byId(op[1]);
              if (!parent) break;
              const existing = this.dm.byId(op[2].id ?? '');
              if (existing) existing.setParent(null);
              const inst = deserializeInstance(op[2], report, true);
              if (inst) inst.setParent(parent, op[3] >= 0 ? op[3] : undefined);
              break;
            }
            case 'd': { const i = this.dm.byId(op[1]); if (i && !i.locked) i.setParent(null); break; }
            case 'm': { const i = this.dm.byId(op[1]), p = this.dm.byId(op[2]); if (i && p && !i.locked && !i.canParentTo(p)) i.setParent(p); break; }
            case 'p': {
              const i = this.dm.byId(op[1]);
              if (!i) break;
              const r = sanitizeValue(i.className, op[2], op[3], report);
              if (r.ok) i.setSilent(op[2], r.value);
              break;
            }
            case 'a': { const i = this.dm.byId(op[1]); if (i) i.setAttribute(op[2], op[3] ?? undefined); break; }
            case 'x': {
              for (const t of op[1]) {
                const i = this.dm.byId(String(t[0]));
                if (!i || !i.isA('BasePart')) continue;
                i.props.Position = v3(+t[1], +t[2], +t[3]);
                i.props.Orientation = v3(+t[4], +t[5], +t[6]);
                moved.push(i);
              }
              break;
            }
          }
        } catch (e) { console.warn('replication op failed', op, e); }
      }
      if (moved.length) this.dm.transformsChanged.fire(moved);
    } finally { this.applying = false; }
    return moved;
  }

  /** Replaces the replicated content with a host snapshot. */
  applySnapshot(items: { parent: string; json: InstanceJSON }[]) {
    const report = new LoadReport();
    this.applying = true;
    try {
      for (const root of this.roots) {
        for (const c of [...root.children]) {
          if (c.locked || isCharacterInstance(c) || c.className === 'Player' || c.className === 'Camera') continue;
          c.setParent(null);
        }
        if (root.className === 'Players') for (const p of root.children) for (const pc of [...p.children]) if (!isLocalOnly(pc)) pc.setParent(null);
      }
      for (const it of items) {
        const parent = this.dm.byId(it.parent);
        if (!parent) continue;
        const existing = this.dm.byId(it.json.id ?? '');
        if (existing) continue;
        const inst = deserializeInstance(it.json, report, true);
        if (inst) inst.setParent(parent);
      }
    } finally { this.applying = false; }
  }

  dispose() { this.unsub.forEach((u) => u()); this.unsub = []; }
}

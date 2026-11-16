// JS half of the Lua scripting runtime (Lua 5.4 via wasmoon/WebAssembly).
// Exposes the scene to scripts through a small bridge; see prelude.lua.ts and
// docs/SCRIPTING_API.md.

import { LuaFactory, type LuaEngine } from 'wasmoon';
import wasmUrl from 'wasmoon/dist/glue.wasm?url';
import * as THREE from 'three';
import { DataModel, Instance } from '../core/Instance';
import { classEvents, classMethods, getClass, getPropDef } from '../core/classes';
import { hexToRgb, isUDim2, isVec3, rgbToHex, udim2, v3, type UDim2, type Vec3 } from '../core/types';
import { ENUMS, enumTypeFor } from './enums';
import { PRELUDE } from './luaPrelude';
import { boundsOf, orientationToQuat, quatToOrientation } from '../render/geometry';

let factory: LuaFactory | null = null;
function getFactory() { return (factory ??= new LuaFactory(wasmUrl)); }

export type Enc = unknown;

/** Hooks the runtime host (play session or edit-mode command bar) provides. */
export interface RuntimeHost {
  log(level: 'print' | 'warn' | 'error' | 'info', message: string, scriptId?: string, line?: number): void;
  /** Return `undefined` when not handled. */
  getProp?(inst: Instance, key: string): { value: Enc } | undefined;
  setProp?(inst: Instance, key: string, value: unknown): boolean;
  call?(inst: Instance, method: string, args: unknown[]): { value: Enc } | undefined;
  raycast?(origin: Vec3, dir: Vec3, filter: Instance[], mode: 'Include' | 'Exclude'): Enc;
  /** Wraps scene mutations (used for undo in edit mode). */
  mutate?<T>(fn: () => T): T;
}

const RUNTIME_SERVICES = new Set(['RunService', 'UserInputService', 'TweenService', 'Debris', 'DataStoreService']);

/** Converts simple Luau syntax that Lua 5.4 lacks (compound assignment). */
export function preprocessLuau(src: string): string {
  return src.split('\n').map((line) => {
    const m = line.match(/^(\s*)([A-Za-z_][\w]*(?:\.[A-Za-z_]\w*|\[[^\]]+\])*)\s*(\+|-|\*|\/\/|\/|%|\^|\.\.)=\s*(.*)$/);
    if (!m) return line;
    let rest = m[4];
    let comment = '';
    let q: string | null = null;
    for (let i = 0; i < rest.length; i++) {
      const c = rest[i];
      if (q) { if (c === '\\') i++; else if (c === q) q = null; continue; }
      if (c === '"' || c === "'") q = c;
      else if (c === '-' && rest[i + 1] === '-') { comment = ' ' + rest.slice(i); rest = rest.slice(0, i).trimEnd(); break; }
    }
    if (!rest) return line;
    return `${m[1]}${m[2]} = ${m[2]} ${m[3]} (${rest})${comment}`;
  }).join('\n');
}

export class LuaRuntime {
  private engine!: LuaEngine;
  private known = new Map<string, Instance>();
  private connected = new Set<string>();
  private events: unknown[][] = [];
  private stepFn!: (dt: number, t: number, events: unknown[][]) => void;
  private runFn!: (id: string, cls: string, name: string, src: string) => boolean;
  private stopFn!: (id: string) => void;
  private execFn!: (src: string) => boolean;
  readonly running = new Set<string>();
  private primaryParts = new WeakMap<Instance, Instance>();
  private disposed = false;
  private unsub: (() => void)[] = [];

  private constructor(readonly dm: DataModel, readonly host: RuntimeHost) {}

  static async create(dm: DataModel, host: RuntimeHost): Promise<LuaRuntime> {
    const rt = new LuaRuntime(dm, host);
    const engine = await getFactory().createEngine({ injectObjects: false, enableProxy: false, openStandardLibs: true });
    rt.engine = engine;
    const g = engine.global;
    const guard = <A extends unknown[], R>(fn: (...a: A) => R) => (...a: A): R => {
      try { return fn(...a); } catch (e) { throw new Error((e as Error).message ?? String(e)); }
    };
    g.set('__B_get', guard((id: string, key: string) => rt.bridgeGet(id, key)));
    g.set('__B_set', guard((id: string, key: string, value: unknown) => { rt.bridgeSet(id, key, value); return undefined; }));
    g.set('__B_call', guard((id: string, method: string, ...args: unknown[]) => rt.bridgeCall(id, method, args)));
    g.set('__B_new', guard((cls: string) => rt.bridgeNew(cls)));
    g.set('__B_connect', (id: string, event: string) => { rt.connected.add(id + '|' + event); return undefined; });
    g.set('__B_kind', (cls: string, key: string) => rt.kind(cls, key));
    g.set('__B_log', (level: string, msg: string, sid?: string, line?: number) => { rt.host.log(level as 'print', String(msg), sid ?? undefined, line ?? undefined); return undefined; });
    g.set('__B_enums', () => ENUMS);
    g.set('__B_clock', () => performance.now() / 1000);
    g.set('__GAME_ID', dm.id);
    rt.register(dm);
    // sandbox: no file/process access from game scripts
    engine.doStringSync('io = nil; os.exit = nil; os.execute = nil; os.remove = nil; os.rename = nil; os.tmpname = nil; os.getenv = nil; dofile = nil; loadfile = nil; package.loadlib = nil');
    engine.doStringSync(PRELUDE);
    rt.stepFn = g.get('__step');
    rt.runFn = g.get('__runScript');
    rt.stopFn = g.get('__stopScript');
    rt.execFn = g.get('__exec');
    rt.bindEvents();
    return rt;
  }

  // ---- encoding --------------------------------------------------------------------

  private register(i: Instance) { this.known.set(i.id, i); }

  encInst(i: Instance | null | undefined): Enc {
    if (!i) return undefined;
    this.register(i);
    return ['I', i.id, i.className];
  }
  encV3(v: Vec3): Enc { return ['V3', v.x, v.y, v.z]; }
  encColor(hex: string): Enc { const [r, g, b] = hexToRgb(hex); return ['C3', r, g, b]; }
  encCFrame(pos: Vec3, orient: Vec3): Enc {
    const m = new THREE.Matrix4().makeRotationFromQuaternion(orientationToQuat(orient));
    const e = m.elements; // column-major
    return ['CF', pos.x, pos.y, pos.z, e[0], e[4], e[8], e[1], e[5], e[9], e[2], e[6], e[10]];
  }

  encodeProp(inst: Instance, prop: string, v: unknown): Enc {
    const def = getPropDef(inst.className, prop);
    if (!def) return v as Enc;
    switch (def.type) {
      case 'vec3': return this.encV3(v as Vec3);
      case 'color': return this.encColor(v as string);
      case 'udim2': { const u = v as UDim2; return ['U2', u.xs, u.xo, u.ys, u.yo]; }
      case 'enum': return ['E', enumTypeFor(inst.className, prop), v];
      default: return v as Enc;
    }
  }

  private inst(id: string): Instance {
    const i = this.dm.byId(id) ?? this.known.get(id);
    if (!i) throw new Error('Object no longer exists');
    return i;
  }

  decodeInst(v: unknown): Instance | null {
    if (Array.isArray(v) && v[0] === 'I') return this.inst(v[1]);
    return null;
  }
  decodeVec3(v: unknown, what: string): Vec3 {
    if (Array.isArray(v) && v[0] === 'V3') return v3(+v[1], +v[2], +v[3]);
    throw new Error(`${what}: Vector3 expected, got ${describe(v)}`);
  }
  decodeCFrame(v: unknown): { pos: Vec3; orient: Vec3 } {
    if (!(Array.isArray(v) && v[0] === 'CF')) throw new Error(`CFrame expected, got ${describe(v)}`);
    const [, x, y, z, r00, r01, r02, r10, r11, r12, r20, r21, r22] = v as number[];
    const m = new THREE.Matrix4().set(r00, r01, r02, 0, r10, r11, r12, 0, r20, r21, r22, 0, 0, 0, 0, 1);
    return { pos: v3(x, y, z), orient: quatToOrientation(new THREE.Quaternion().setFromRotationMatrix(m)) };
  }

  private decodeProp(inst: Instance, prop: string, v: unknown): unknown {
    const def = getPropDef(inst.className, prop)!;
    const fail = (exp: string) => { throw new Error(`Unable to assign property ${prop}. ${exp} expected, got ${describe(v)}`); };
    switch (def.type) {
      case 'number': if (typeof v !== 'number' || Number.isNaN(v)) fail('number'); return v;
      case 'int': if (typeof v !== 'number') fail('number'); return Math.round(v as number);
      case 'bool': if (typeof v !== 'boolean') fail('boolean'); return v;
      case 'string': case 'source': if (typeof v === 'number') return String(v); if (typeof v !== 'string') fail('string'); return v;
      case 'asset': if (typeof v !== 'string') fail('string'); return v;
      case 'vec3': return this.decodeVec3(v, `Unable to assign property ${prop}`);
      case 'color': if (Array.isArray(v) && v[0] === 'C3') return rgbToHex(+v[1], +v[2], +v[3]); return fail('Color3');
      case 'udim2': if (Array.isArray(v) && v[0] === 'U2') return udim2(+v[1], +v[2], +v[3], +v[4]); return fail('UDim2');
      case 'enum': {
        const name = Array.isArray(v) && v[0] === 'E' ? String(v[2]) : typeof v === 'string' ? v : typeof v === 'number' ? def.enumValues![v] : undefined;
        if (!name || !def.enumValues!.includes(name)) fail(`Enum.${enumTypeFor(inst.className, prop)}`);
        return name;
      }
    }
  }

  // ---- bridge --------------------------------------------------------------------

  private kindCache = new Map<string, string>();
  private kind(cls: string, key: string): string {
    const ck = cls + '.' + key;
    let k = this.kindCache.get(ck);
    if (k !== undefined) return k;
    if (getPropDef(cls, key) || key === 'Parent') k = 'p';
    else if (classMethods(cls).includes(key)) k = 'm';
    else if (classEvents(cls).includes(key)) k = 'e';
    else k = '';
    this.kindCache.set(ck, k);
    return k;
  }

  private bridgeGet(id: string, key: string): Enc {
    const inst = this.inst(id);
    const h = this.host.getProp?.(inst, key);
    if (h) return h.value;
    if (key === 'Parent') return this.encInst(inst.parent);
    if (inst.isA('BasePart') || inst.className === 'Camera') {
      if (key === 'CFrame') return this.encCFrame(inst.props.Position as Vec3, inst.props.Orientation as Vec3);
    }
    if (inst.className === 'Workspace' && key === 'CurrentCamera') return this.encInst(inst.findFirstChildOfClass('Camera'));
    if (inst.className === 'Model' && key === 'PrimaryPart') return this.encInst(this.primaryParts.get(inst) ?? null);
    if (key in inst.props) return this.encodeProp(inst, key, inst.props[key]);
    const child = inst.findFirstChild(key);
    if (child) return this.encInst(child);
    throw new Error(`${key} is not a valid member of ${inst.className} "${inst.getFullName() || inst.name}"`);
  }

  private bridgeSet(id: string, key: string, value: unknown) {
    const inst = this.inst(id);
    if (this.host.setProp?.(inst, key, value)) return;
    const mut = <T>(fn: () => T) => (this.host.mutate ? this.host.mutate(fn) : fn());
    if (key === 'Parent') {
      const p = value === undefined || value === null ? null : this.decodeInst(value);
      if (value !== undefined && value !== null && !p) throw new Error(`Unable to assign property Parent. Instance expected, got ${describe(value)}`);
      if (inst.locked) throw new Error(`The Parent property of ${inst.name} is locked`);
      const err = inst.canParentTo(p);
      if (err) throw new Error(err);
      mut(() => inst.setParent(p));
      return;
    }
    if ((inst.isA('BasePart') || inst.className === 'Camera') && key === 'CFrame') {
      const { pos, orient } = this.decodeCFrame(value);
      mut(() => { inst.set('Position', pos); inst.set('Orientation', orient); });
      return;
    }
    if (inst.className === 'Model' && key === 'PrimaryPart') {
      const p = this.decodeInst(value);
      if (p) this.primaryParts.set(inst, p); else this.primaryParts.delete(inst);
      return;
    }
    const def = getPropDef(inst.className, key);
    if (!def) throw new Error(`${key} is not a valid member of ${inst.className} "${inst.getFullName() || inst.name}"`);
    if (def.readOnly) throw new Error(`Unable to assign property ${key}. Property is read only`);
    if (key === 'Name' && inst.locked) throw new Error(`Cannot rename ${inst.className}`);
    const v = this.decodeProp(inst, key, value);
    mut(() => inst.set(key, v));
  }

  private bridgeNew(cls: string): string {
    const def = getClass(cls);
    if (!def || !def.creatable) throw new Error(`Unable to create an Instance of type "${cls}"`);
    const i = new Instance(cls);
    this.register(i);
    return i.id;
  }

  private list(items: Instance[]): Enc { return ['T', ...items.map((i) => this.encInst(i))]; }

  private bridgeCall(id: string, method: string, args: unknown[]): Enc {
    const inst = this.inst(id);
    const h = this.host.call?.(inst, method, args);
    if (h) return h.value;
    const str = (k: number) => { const a = args[k]; if (typeof a !== 'string') throw new Error(`${method}: argument ${k + 1} must be a string`); return a; };
    const mut = <T>(fn: () => T) => (this.host.mutate ? this.host.mutate(fn) : fn());
    switch (method) {
      case '__moduleSource': {
        if (inst.className !== 'ModuleScript') throw new Error('require expects a ModuleScript');
        return ['M', preprocessLuau(inst.props.Source as string), inst.getFullName()];
      }
      case 'Destroy':
        if (inst.locked) throw new Error(`${inst.className} cannot be destroyed`);
        this.fire(inst, 'Destroying');
        mut(() => inst.setParent(null));
        return undefined;
      case 'Clone': { const c = inst.clone(); this.register(c); c.walk((d) => { this.register(d); }); return this.encInst(c); }
      case 'FindFirstChild': return this.encInst(inst.findFirstChild(str(0), args[1] === true));
      case 'FindFirstChildOfClass': return this.encInst(inst.findFirstChildOfClass(str(0)));
      case 'FindFirstChildWhichIsA': return this.encInst(inst.findFirstChildWhichIsA(str(0)));
      case 'FindFirstAncestor': { const n = str(0); let p = inst.parent; while (p && p.name !== n) p = p.parent; return this.encInst(p); }
      case 'FindFirstAncestorOfClass': return this.encInst(inst.findFirstAncestorOfClass(str(0)));
      case 'FindFirstAncestorWhichIsA': { const n = str(0); let p = inst.parent; while (p && !p.isA(n)) p = p.parent; return this.encInst(p); }
      case 'GetChildren': return this.list(inst.children);
      case 'GetDescendants': return this.list(inst.getDescendants());
      case 'IsA': return inst.isA(str(0));
      case 'IsDescendantOf': { const o = this.decodeInst(args[0]); return !!o && inst.isDescendantOf(o); }
      case 'IsAncestorOf': { const o = this.decodeInst(args[0]); return !!o && o.isDescendantOf(inst); }
      case 'GetFullName': return inst.getFullName();
      case 'ClearAllChildren': mut(() => { for (const c of [...inst.children]) if (!c.locked) c.setParent(null); }); return undefined;
      case 'GetAttribute': { const v = inst.attributes[str(0)]; return encodeAttr(v, this); }
      case 'SetAttribute': mut(() => inst.setAttribute(str(0), decodeAttr(args[1], this))); return undefined;
      case 'GetService': {
        const name = str(0);
        if (!(inst instanceof DataModel)) throw new Error('GetService is only valid on game');
        let s = inst.findFirstChildOfClass(name);
        if (!s && RUNTIME_SERVICES.has(name)) {
          s = new Instance(name);
          s.props.Name = name;
          s.runtimeOnly = true;
          s.setParent(inst);
        }
        if (!s) throw new Error(`'${name}' is not a valid Service name`);
        return this.encInst(s);
      }
      case 'GetPlayers': return this.list(inst.children.filter((c) => c.className === 'Player'));
      case 'GetPlayerFromCharacter': return undefined; // overridden by the play session host
      case 'GetPartsInRadius': {
        const c = this.decodeVec3(args[0], 'GetPartsInRadius'), r = Number(args[1]);
        const out = inst.getDescendants().filter((d) => d.isA('BasePart') && dist(d.props.Position as Vec3, c) <= r);
        return this.list(out);
      }
      case 'GetBoundingBox': case 'GetPivot': case 'PivotTo': case 'MoveTo': return this.modelOp(inst, method, args);
      case 'LookAt': {
        const target = this.decodeVec3(args[0], 'LookAt');
        const pos = inst.props.Position as Vec3;
        const m = new THREE.Matrix4().lookAt(new THREE.Vector3(pos.x, pos.y, pos.z), new THREE.Vector3(target.x, target.y, target.z), new THREE.Vector3(0, 1, 0));
        mut(() => inst.set('Orientation', quatToOrientation(new THREE.Quaternion().setFromRotationMatrix(m))));
        return undefined;
      }
      case 'IsRunning': case 'IsStudio': case 'IsClient': case 'IsServer': return true;
      case 'Raycast': {
        if (!this.host.raycast) throw new Error('Raycast is only available while the game is running');
        const o = this.decodeVec3(args[0], 'Raycast origin'), d = this.decodeVec3(args[1], 'Raycast direction');
        const filter = Array.isArray(args[2]) ? (args[2] as unknown[]).slice(1).map((x) => this.decodeInst(x)).filter((x): x is Instance => !!x) : [];
        return this.host.raycast(o, d, filter, args[3] === 'Include' ? 'Include' : 'Exclude');
      }
    }
    throw new Error(`${method} is not supported on ${inst.className} in this runtime`);
  }

  private modelOp(inst: Instance, method: string, args: unknown[]): Enc {
    const parts = inst.isA('BasePart') ? [inst] : inst.getDescendants().filter((d) => d.isA('BasePart'));
    const mut = <T>(fn: () => T) => (this.host.mutate ? this.host.mutate(fn) : fn());
    const primary = this.primaryParts.get(inst);
    const pivotOf = () => {
      if (primary) return { pos: primary.props.Position as Vec3, orient: primary.props.Orientation as Vec3 };
      if (inst.isA('BasePart')) return { pos: inst.props.Position as Vec3, orient: inst.props.Orientation as Vec3 };
      const c = boundsOf(parts).getCenter(new THREE.Vector3());
      return { pos: v3(c.x, c.y, c.z), orient: v3() };
    };
    switch (method) {
      case 'GetBoundingBox': {
        const b = boundsOf(parts);
        const c = b.getCenter(new THREE.Vector3()), s = b.getSize(new THREE.Vector3());
        return ['M', this.encCFrame(v3(c.x, c.y, c.z), v3()), this.encV3(v3(s.x, s.y, s.z))];
      }
      case 'GetPivot': { const p = pivotOf(); return this.encCFrame(p.pos, p.orient); }
      case 'PivotTo': case 'MoveTo': {
        const from = pivotOf();
        const fromM = new THREE.Matrix4().compose(new THREE.Vector3(from.pos.x, from.pos.y, from.pos.z), orientationToQuat(from.orient), new THREE.Vector3(1, 1, 1));
        let toM: THREE.Matrix4;
        if (method === 'PivotTo') {
          const { pos, orient } = this.decodeCFrame(args[0]);
          toM = new THREE.Matrix4().compose(new THREE.Vector3(pos.x, pos.y, pos.z), orientationToQuat(orient), new THREE.Vector3(1, 1, 1));
        } else {
          const p = this.decodeVec3(args[0], 'MoveTo');
          toM = fromM.clone().setPosition(p.x, p.y, p.z);
        }
        const delta = toM.multiply(fromM.clone().invert());
        const pos = new THREE.Vector3(), q = new THREE.Quaternion(), s = new THREE.Vector3();
        mut(() => {
          for (const part of parts) {
            const m = new THREE.Matrix4().compose(new THREE.Vector3().copy(part.props.Position as THREE.Vector3Like), orientationToQuat(part.props.Orientation as Vec3), new THREE.Vector3(1, 1, 1));
            delta.clone().multiply(m).decompose(pos, q, s);
            part.set('Position', v3(pos.x, pos.y, pos.z));
            part.set('Orientation', quatToOrientation(q));
          }
        });
        return undefined;
      }
    }
    return undefined;
  }

  // ---- events --------------------------------------------------------------------

  isConnected(inst: Instance, event: string) { return this.connected.has(inst.id + '|' + event); }

  private asyncCounter = 0;
  /** Starts an async operation; the Lua thread yields until `promise` settles. */
  startAsync(promise: Promise<Enc>): Enc {
    const token = 'a' + (++this.asyncCounter);
    promise.then(
      (value) => this.events.push(['__async', token, true, value === undefined || value === null ? ['N'] : value]),
      (err) => this.events.push(['__async', token, false, String((err as Error)?.message ?? err)]),
    );
    return ['ASYNC', token];
  }

  /** Queues an event for delivery on the next step. Args must already be encoded. */
  fire(inst: Instance, event: string, ...args: Enc[]) {
    if (!this.connected.has(inst.id + '|' + event)) return;
    this.events.push([inst.id, event, ...args.map((a) => (a === undefined || a === null ? ['N'] : a))]);
  }

  private bindEvents() {
    const dm = this.dm;
    this.unsub.push(
      dm.changed.connect((i, prop) => { if (this.connected.has(i.id + '|Changed')) this.fire(i, 'Changed', prop); }),
      dm.childAdded.connect((p, c) => this.fire(p, 'ChildAdded', this.encInst(c))),
      dm.childRemoved.connect((p, c) => this.fire(p, 'ChildRemoved', this.encInst(c))),
      dm.reparented.connect((i) => this.fire(i, 'AncestryChanged', this.encInst(i), this.encInst(i.parent))),
      dm.removed.connect((i, oldParent) => {
        if (i.isA('LuaSourceContainer') && this.running.has(i.id)) this.stopScript(i);
        if (oldParent) this.fire(i, 'AncestryChanged', this.encInst(i), undefined);
      }),
    );
  }

  // ---- scripts ---------------------------------------------------------------------

  runScript(inst: Instance): boolean {
    if (this.disposed) return false;
    if (this.running.has(inst.id)) return false;
    this.running.add(inst.id);
    this.register(inst);
    return this.runFn(inst.id, inst.className, inst.getFullName(), preprocessLuau(inst.props.Source as string));
  }

  stopScript(inst: Instance) {
    if (!this.running.delete(inst.id)) return;
    this.stopFn(inst.id);
  }

  /** Runs one frame of the scheduler, delivering queued events. */
  step(dt: number, time: number) {
    if (this.disposed) return;
    const events = this.events;
    this.events = [];
    try {
      this.stepFn(dt, time, events);
    } catch (e) {
      this.host.log('error', `Script runtime error: ${(e as Error).message}`);
    }
  }

  exec(code: string) {
    try { this.execFn(preprocessLuau(code)); } catch (e) { this.host.log('error', (e as Error).message); }
  }

  /** Syntax check without running (used by the script editor). */
  static async checkSyntax(src: string): Promise<{ line: number; message: string } | null> {
    const eng = await getChecker();
    const fn = eng.global.get('__check') as (s: string) => string | undefined;
    const err = fn(preprocessLuau(src));
    if (!err) return null;
    const m = String(err).match(/^[^:]*:(\d+):\s*(.*)$/s);
    return m ? { line: +m[1], message: m[2] } : { line: 1, message: String(err) };
  }

  dispose() {
    if (this.disposed) return;
    this.disposed = true;
    this.unsub.forEach((u) => u());
    try { this.engine.global.close(); } catch { /* already closed */ }
  }
}

let checker: Promise<LuaEngine> | null = null;
function getChecker() {
  return (checker ??= (async () => {
    const e = await getFactory().createEngine({ injectObjects: false, enableProxy: false });
    e.doStringSync('function __check(src) local f, err = load(src, "=script", "t") if f then return nil end return err end');
    return e;
  })());
}

function describe(v: unknown): string {
  if (v === undefined || v === null) return 'nil';
  if (Array.isArray(v)) {
    const t = v[0];
    return t === 'V3' ? 'Vector3' : t === 'C3' ? 'Color3' : t === 'U2' ? 'UDim2' : t === 'CF' ? 'CFrame' : t === 'I' ? 'Instance' : t === 'E' ? 'EnumItem' : 'table';
  }
  return typeof v === 'object' ? 'table' : typeof v;
}

function dist(a: Vec3, b: Vec3) { return Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z); }

function encodeAttr(v: unknown, rt: LuaRuntime): Enc {
  if (isVec3(v)) return rt.encV3(v);
  if (isUDim2(v)) return ['U2', v.xs, v.xo, v.ys, v.yo];
  if (typeof v === 'string' && v.startsWith('#color:')) return rt.encColor(v.slice(7));
  return v as Enc;
}
function decodeAttr(v: unknown, rt: LuaRuntime): unknown {
  if (Array.isArray(v)) {
    if (v[0] === 'V3') return rt.decodeVec3(v, 'SetAttribute');
    if (v[0] === 'C3') return '#color:' + rgbToHex(+v[1], +v[2], +v[3]);
    if (v[0] === 'U2') return udim2(+v[1], +v[2], +v[3], +v[4]);
    throw new Error('SetAttribute: unsupported value type');
  }
  if (v === false || typeof v === 'number' || typeof v === 'string' || v === true || v === undefined) return v;
  throw new Error('SetAttribute: unsupported value type');
}

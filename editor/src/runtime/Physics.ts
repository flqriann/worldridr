// Rigid-body physics for play mode using Rapier (Rust, compiled to WASM).
// Every BasePart in Workspace gets a body: anchored parts are fixed (switched
// to kinematic once a script moves them, so they carry/push correctly),
// others are dynamic. CanCollide=false parts are sensors. Contact and sensor
// events become Touched/TouchEnded; terrain becomes a heightfield.

import RAPIER from '@dimforge/rapier3d-compat';
import * as THREE from 'three';
import type { DataModel, Instance } from '../core/Instance';
import { v3, type Vec3 } from '../core/types';
import { effectiveSize, orientationToQuat, quatToOrientation, shapeOf } from '../render/geometry';

export { RAPIER };

let ready: Promise<void> | null = null;
/** Loads the Rapier WASM module (once). */
export function initPhysics(): Promise<void> {
  return (ready ??= RAPIER.init());
}

export interface PartBody { inst: Instance; body: RAPIER.RigidBody; collider: RAPIER.Collider; anchored: boolean }

const FIXED_DT = 1 / 60;

function wedgePoints(s: Vec3): Float32Array {
  const x = s.x / 2, y = s.y / 2, z = s.z / 2;
  return new Float32Array([-x, -y, -z, x, -y, -z, x, -y, z, -x, -y, z, -x, y, z, x, y, z]);
}

export class Physics {
  readonly world: RAPIER.World;
  readonly bodies = new Map<Instance, PartBody>();
  /** collider handle -> instance */
  readonly byCollider = new Map<number, Instance>();
  private terrainCollider: RAPIER.Collider | null = null;
  private events = new RAPIER.EventQueue(true);
  private accumulator = 0;
  private contacts = new Map<string, [Instance, Instance]>();
  onTouch: (a: Instance, b: Instance, began: boolean) => void = () => {};
  /** Instances driven externally (character parts): never get bodies, never synced. */
  readonly kinematic = new Set<Instance>();
  private suppress = 0;

  /**
   * `authoritative = false` (multiplayer clients): nothing is simulated locally;
   * unanchored parts become kinematic bodies that follow the host's transforms.
   */
  constructor(private dm: DataModel, readonly authoritative = true) {
    this.world = new RAPIER.World({ x: 0, y: -(dm.workspace.props.Gravity as number), z: 0 });
    this.world.timestep = FIXED_DT;
  }

  build() {
    for (const d of this.dm.workspace.getDescendants()) if (d.isA('BasePart')) this.add(d);
    this.buildTerrain();
  }

  buildTerrain() {
    if (this.terrainCollider) { this.byCollider.delete(this.terrainCollider.handle); this.world.removeCollider(this.terrainCollider, false); this.terrainCollider = null; }
    const t = this.dm.terrainInstance;
    const td = t?.terrain;
    if (!t || !td || !td.hasSolid()) return;
    const n = td.size;
    // Rapier: rows along Z, columns along X, column-major storage.
    const H = td.heights, M = td.materials;
    const data = new Float32Array(n * n);
    for (let ix = 0; ix < n; ix++) for (let iz = 0; iz < n; iz++) {
      const k = td.idx(ix, iz);
      data[iz + ix * n] = M[k] ? H[k] : -200;
    }
    const desc = RAPIER.ColliderDesc.heightfield(n - 1, n - 1, data, { x: td.extent, y: 1, z: td.extent }).setFriction(0.6);
    this.terrainCollider = this.world.createCollider(desc);
    this.byCollider.set(this.terrainCollider.handle, t);
  }

  add(inst: Instance) {
    if (this.kinematic.has(inst)) return;
    if (this.bodies.has(inst)) this.remove(inst);
    const p = inst.props;
    const s = effectiveSize(inst);
    const anchored = !!p.Anchored;
    const simulated = !anchored && this.authoritative;
    const pos = p.Position as Vec3;
    const q = orientationToQuat(p.Orientation as Vec3);
    const bodyDesc = (anchored ? RAPIER.RigidBodyDesc.fixed() : simulated ? RAPIER.RigidBodyDesc.dynamic() : RAPIER.RigidBodyDesc.kinematicPositionBased())
      .setTranslation(pos.x, pos.y, pos.z)
      .setRotation({ x: q.x, y: q.y, z: q.z, w: q.w })
      .setCcdEnabled(simulated && Math.min(s.x, s.y, s.z) < 1.5)
      .setLinearDamping(0.02).setAngularDamping(0.05);
    const body = this.world.createRigidBody(bodyDesc);
    if (simulated) { const vel = p.AssemblyLinearVelocity as Vec3; body.setLinvel({ x: vel.x, y: vel.y, z: vel.z }, true); }

    const kind = inst.className === 'MeshPart' ? 'Block' : shapeOf(inst);
    let cd: RAPIER.ColliderDesc | null;
    switch (kind) {
      case 'Ball': cd = RAPIER.ColliderDesc.ball(s.x / 2); break;
      case 'Cylinder':
        // Rapier cylinders run along Y; our cylinders run along X
        cd = RAPIER.ColliderDesc.cylinder(s.x / 2, s.y / 2).setRotation({ x: 0, y: 0, z: Math.SQRT1_2, w: Math.SQRT1_2 });
        break;
      case 'Wedge': cd = RAPIER.ColliderDesc.convexHull(wedgePoints(s)); break;
      default: cd = RAPIER.ColliderDesc.cuboid(Math.max(s.x, 0.05) / 2, Math.max(s.y, 0.05) / 2, Math.max(s.z, 0.05) / 2);
    }
    cd ??= RAPIER.ColliderDesc.cuboid(s.x / 2, s.y / 2, s.z / 2);
    cd.setDensity(Math.max(0.01, p.Density as number))
      .setFriction(p.Friction as number)
      .setRestitution(p.Elasticity as number)
      .setSensor(!p.CanCollide)
      .setActiveEvents(RAPIER.ActiveEvents.COLLISION_EVENTS)
      .setActiveCollisionTypes(RAPIER.ActiveCollisionTypes.DEFAULT | RAPIER.ActiveCollisionTypes.KINEMATIC_FIXED | RAPIER.ActiveCollisionTypes.KINEMATIC_KINEMATIC);
    const collider = this.world.createCollider(cd, body);
    this.bodies.set(inst, { inst, body, collider, anchored: !simulated });
    this.byCollider.set(collider.handle, inst);
  }

  remove(inst: Instance) {
    const pb = this.bodies.get(inst);
    if (!pb) return;
    this.byCollider.delete(pb.collider.handle);
    this.world.removeRigidBody(pb.body);
    this.bodies.delete(inst);
    for (const [k, [a, b]] of this.contacts) if (a === inst || b === inst) this.contacts.delete(k);
  }

  /** Called when a part property changed from outside physics (scripts, tweens, property panel). */
  onPropChanged(inst: Instance, prop: string) {
    if (this.suppress) return;
    const pb = this.bodies.get(inst);
    if (!pb) return;
    const b = pb.body;
    switch (prop) {
      case 'Position': case 'Orientation': {
        const p = inst.props.Position as Vec3;
        const q = orientationToQuat(inst.props.Orientation as Vec3);
        if (pb.anchored) {
          // scripted/animated anchored parts become kinematic so they push and carry bodies
          if (!b.isKinematic()) b.setBodyType(RAPIER.RigidBodyType.KinematicPositionBased, true);
          b.setNextKinematicTranslation({ x: p.x, y: p.y, z: p.z });
          b.setNextKinematicRotation({ x: q.x, y: q.y, z: q.z, w: q.w });
        } else {
          b.setTranslation({ x: p.x, y: p.y, z: p.z }, true);
          b.setRotation({ x: q.x, y: q.y, z: q.z, w: q.w }, true);
        }
        break;
      }
      case 'AssemblyLinearVelocity': { const v = inst.props.AssemblyLinearVelocity as Vec3; if (!pb.anchored) b.setLinvel({ x: v.x, y: v.y, z: v.z }, true); break; }
      case 'CanCollide': pb.collider.setSensor(!inst.props.CanCollide); break;
      case 'Anchored': case 'Size': case 'Shape': case 'Density': case 'Friction': case 'Elasticity': this.add(inst); break;
    }
  }

  /** Non-authoritative mode: moves bodies to the transforms received from the host. */
  syncFromInstances(list: Instance[]) {
    for (const inst of list) {
      const pb = this.bodies.get(inst);
      if (!pb) continue;
      const p = inst.props.Position as Vec3;
      const q = orientationToQuat(inst.props.Orientation as Vec3);
      if (pb.body.isKinematic()) {
        pb.body.setNextKinematicTranslation({ x: p.x, y: p.y, z: p.z });
        pb.body.setNextKinematicRotation({ x: q.x, y: q.y, z: q.z, w: q.w });
      } else {
        pb.body.setTranslation({ x: p.x, y: p.y, z: p.z }, false);
        pb.body.setRotation({ x: q.x, y: q.y, z: q.z, w: q.w }, false);
      }
    }
  }

  setVelocity(inst: Instance, v: Vec3) { const pb = this.bodies.get(inst); if (pb && !pb.anchored) pb.body.setLinvel({ x: v.x, y: v.y, z: v.z }, true); }
  getVelocity(inst: Instance): Vec3 | null { const pb = this.bodies.get(inst); if (!pb) return null; const l = pb.body.linvel(); return v3(l.x, l.y, l.z); }
  applyImpulse(inst: Instance, imp: Vec3) { const pb = this.bodies.get(inst); if (pb && !pb.anchored) pb.body.applyImpulse({ x: imp.x, y: imp.y, z: imp.z }, true); }
  getMass(inst: Instance) { return this.bodies.get(inst)?.body.mass() ?? 0; }

  touchingParts(inst: Instance): Instance[] {
    const out = new Set<Instance>();
    for (const [a, b] of this.contacts.values()) { if (a === inst) out.add(b); else if (b === inst) out.add(a); }
    return [...out];
  }

  /** Records a begin/end touch and notifies listeners once per change. */
  trackTouch(a: Instance, b: Instance, began: boolean) {
    const key = a.id < b.id ? `${a.id}|${b.id}` : `${b.id}|${a.id}`;
    const had = this.contacts.has(key);
    if (began === had) return;
    if (began) this.contacts.set(key, [a, b]); else this.contacts.delete(key);
    if (a.props.CanTouch === false || b.props.CanTouch === false) return;
    this.onTouch(a, b, began);
  }

  /** Advances the simulation with a fixed 60 Hz step (max 4 sub-steps per frame). */
  step(dt: number) {
    const g = this.dm.workspace.props.Gravity as number;
    this.world.gravity = { x: 0, y: -g, z: 0 };
    this.accumulator = Math.min(this.accumulator + dt, FIXED_DT * 4);
    while (this.accumulator >= FIXED_DT) {
      this.world.step(this.events);
      this.accumulator -= FIXED_DT;
      this.events.drainCollisionEvents((h1, h2, started) => {
        const a = this.byCollider.get(h1), b = this.byCollider.get(h2);
        if (a && b) this.trackTouch(a, b, started);
      });
    }
  }

  /** Writes simulated transforms of dynamic bodies back into the instances. */
  syncToInstances() {
    const q = new THREE.Quaternion();
    const moved: Instance[] = [];
    for (const pb of this.bodies.values()) {
      if (pb.anchored || pb.body.isSleeping()) continue;
      const t = pb.body.translation(), r = pb.body.rotation();
      pb.inst.props.Position = v3(round(t.x), round(t.y), round(t.z));
      q.set(r.x, r.y, r.z, r.w);
      pb.inst.props.Orientation = quatToOrientation(q);
      moved.push(pb.inst);
    }
    if (moved.length) this.dm.transformsChanged.fire(moved);
  }

  /** First solid hit along origin + dir (dir length = max distance). Sensors are ignored. */
  raycast(origin: Vec3, dir: Vec3, filter: Set<Instance>, mode: 'Include' | 'Exclude', extra?: (c: RAPIER.Collider) => boolean) {
    const len = Math.hypot(dir.x, dir.y, dir.z);
    if (len < 1e-9) return null;
    const ray = new RAPIER.Ray({ x: origin.x, y: origin.y, z: origin.z }, { x: dir.x / len, y: dir.y / len, z: dir.z / len });
    const list = [...filter];
    const hit = this.world.castRayAndGetNormal(ray, len, true, RAPIER.QueryFilterFlags.EXCLUDE_SENSORS, undefined, undefined, undefined, (c) => {
      if (extra && !extra(c)) return false;
      const inst = this.byCollider.get(c.handle);
      if (!inst) return false;
      const inFilter = list.some((f) => inst === f || inst.isDescendantOf(f));
      return mode === 'Exclude' ? !inFilter : inFilter;
    });
    if (!hit) return null;
    const inst = this.byCollider.get(hit.collider.handle)!;
    const p = ray.pointAt(hit.timeOfImpact);
    return { inst, point: v3(p.x, p.y, p.z), normal: v3(hit.normal.x, hit.normal.y, hit.normal.z), distance: hit.timeOfImpact };
  }

  dispose() {
    this.bodies.clear();
    this.byCollider.clear();
    this.contacts.clear();
    this.events.free();
    this.world.free();
  }
}

function round(x: number) { return Math.round(x * 1000) / 1000; }

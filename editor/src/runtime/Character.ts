// Player character: a blocky R6-style Model with a Humanoid, moved by
// Rapier's kinematic character controller (slopes, steps, snapping to the
// ground, pushing dynamic bodies), plus jumping, swimming, moving-platform
// carry, touch detection and break-apart death.

import * as THREE from 'three';
import { Instance } from '../core/Instance';
import { v3, type Vec3 } from '../core/types';
import { orientationToQuat, quatToOrientation } from '../render/geometry';
import { RAPIER, type Physics } from './Physics';

interface Limb { inst: Instance; offset: THREE.Vector3; pivot: THREE.Vector3; swing: number }

const CAPSULE_HALF = 1.5;   // capsule: 2*1.5 + 2*1 = 5 studs tall
const CAPSULE_RADIUS = 1;
const ROOT_TO_CENTER = -0.5; // root (torso center) is 3 studs above the feet; capsule center 2.5

export class Character {
  readonly model: Instance;
  readonly root: Instance;
  readonly humanoid: Instance;
  readonly head: Instance;
  readonly body: RAPIER.RigidBody;
  readonly collider: RAPIER.Collider;
  private controller: RAPIER.KinematicCharacterController | null = null;
  /** Remote players: driven by network state instead of input. */
  readonly puppet: boolean;
  private target = new THREE.Vector3();
  private targetYaw = 0;
  private limbs: Limb[] = [];
  /** Linear velocity (studs/s) maintained by the controller. */
  readonly velocity = new THREE.Vector3();
  yaw = 0;
  grounded = false;
  swimming = false;
  dead = false;
  private phase = 0;
  private swing = 0;
  private groundPart: Instance | null = null;
  private groundLast: Vec3 | null = null;
  private jumpCooldown = 0;
  private hidden = false;
  private touching = new Set<Instance>();
  private pos = new THREE.Vector3();
  onJump: () => void = () => {};

  /**
   * `idPrefix` makes instance ids deterministic ("char-<playerId>-") so every
   * client and the host refer to the same character parts over the network.
   */
  constructor(name: string, private physics: Physics, starter: Instance, private water: (x: number, z: number) => number | null, opts: { idPrefix: string; puppet?: boolean } = { idPrefix: 'char-0-' }) {
    const sp = starter.props;
    this.puppet = !!opts.puppet;
    const pre = opts.idPrefix;
    const m = new Instance('Model', pre + 'Model'); m.props.Name = name; m.runtimeOnly = true;
    const mk = (n: string, size: Vec3, color: string, shape = 'Block') => {
      const p = new Instance('Part', pre + n.replace(/\s+/g, ''));
      Object.assign(p.props, { Name: n, Size: size, Color: color, Anchored: true, CanCollide: false, Material: 'SmoothPlastic', Shape: shape, CanTouch: true });
      p.runtimeOnly = true;
      p.parent = m; m.children.push(p);
      physics.kinematic.add(p);
      return p;
    };
    this.root = mk('HumanoidRootPart', v3(2, 2, 1), '#000000');
    this.root.props.Transparency = 1;
    this.root.props.CastShadow = false;
    const torso = mk('Torso', v3(2, 2, 1), sp.TorsoColor as string);
    this.head = mk('Head', v3(1.25, 1.25, 1.25), sp.HeadColor as string, 'Ball');
    const la = mk('Left Arm', v3(1, 2, 1), sp.LimbColor as string);
    const ra = mk('Right Arm', v3(1, 2, 1), sp.LimbColor as string);
    const ll = mk('Left Leg', v3(1, 2, 1), sp.LegColor as string);
    const rl = mk('Right Leg', v3(1, 2, 1), sp.LegColor as string);
    const face = mk('Face', v3(0.7, 0.18, 0.05), '#222222');
    face.props.CanTouch = false;
    const hum = new Instance('Humanoid', pre + 'Humanoid');
    hum.runtimeOnly = true;
    Object.assign(hum.props, { Name: 'Humanoid', WalkSpeed: sp.CharacterWalkSpeed, JumpPower: sp.CharacterJumpPower, MaxHealth: sp.CharacterMaxHealth, Health: sp.CharacterMaxHealth });
    hum.parent = m; m.children.push(hum);
    this.model = m;
    this.humanoid = hum;
    this.limbs = [
      { inst: torso, offset: new THREE.Vector3(0, 0, 0), pivot: new THREE.Vector3(), swing: 0 },
      { inst: this.head, offset: new THREE.Vector3(0, 1.6, 0), pivot: new THREE.Vector3(), swing: 0 },
      { inst: face, offset: new THREE.Vector3(0, 1.45, -0.6), pivot: new THREE.Vector3(), swing: 0 },
      { inst: la, offset: new THREE.Vector3(-1.5, 0, 0), pivot: new THREE.Vector3(0, 1, 0), swing: 1 },
      { inst: ra, offset: new THREE.Vector3(1.5, 0, 0), pivot: new THREE.Vector3(0, 1, 0), swing: -1 },
      { inst: ll, offset: new THREE.Vector3(-0.5, -2, 0), pivot: new THREE.Vector3(0, 1, 0), swing: -1 },
      { inst: rl, offset: new THREE.Vector3(0.5, -2, 0), pivot: new THREE.Vector3(0, 1, 0), swing: 1 },
    ];

    const w = physics.world;
    this.body = w.createRigidBody(RAPIER.RigidBodyDesc.kinematicPositionBased());
    this.collider = w.createCollider(
      RAPIER.ColliderDesc.capsule(CAPSULE_HALF, CAPSULE_RADIUS).setTranslation(0, ROOT_TO_CENTER, 0)
        .setActiveEvents(RAPIER.ActiveEvents.COLLISION_EVENTS)
        .setActiveCollisionTypes(RAPIER.ActiveCollisionTypes.ALL),
      this.body,
    );
    physics.byCollider.set(this.collider.handle, this.root);
    if (this.puppet) return;
    const c = w.createCharacterController(0.05);
    c.setUp({ x: 0, y: 1, z: 0 });
    c.setMaxSlopeClimbAngle((50 * Math.PI) / 180);
    c.setMinSlopeSlideAngle((60 * Math.PI) / 180);
    c.enableAutostep(1.1, 0.4, false);
    c.enableSnapToGround(0.7);
    c.setApplyImpulsesToDynamicBodies(true);
    c.setCharacterMass(70);
    this.controller = c;
  }

  /** Places the character with its feet at `feet`. */
  spawn(feet: Vec3, yaw: number) {
    this.yaw = yaw;
    this.pos.set(feet.x, feet.y + 3.05, feet.z);
    this.body.setTranslation(this.pos, true);
    this.velocity.set(0, 0, 0);
    this.writeTransforms(0);
  }

  get position(): THREE.Vector3 { return this.pos; }

  /** Remote players: moves toward the latest network state (smoothed). */
  setRemoteState(p: [number, number, number], yaw: number, swing: number, teleport = false) {
    this.target.set(p[0], p[1], p[2]);
    this.targetYaw = yaw;
    this.swing = swing;
    if (teleport || this.pos.distanceTo(this.target) > 30) { this.pos.copy(this.target); this.yaw = yaw; }
  }

  /** Per-frame interpolation of a puppet. */
  updatePuppet(dt: number) {
    const k = 1 - Math.exp(-14 * dt);
    this.pos.lerp(this.target, k);
    const d = Math.atan2(Math.sin(this.targetYaw - this.yaw), Math.cos(this.targetYaw - this.yaw));
    this.yaw += d * k;
    this.body.setNextKinematicTranslation(this.pos);
    this.writeTransforms(this.swing);
  }

  get currentSwing() { return this.swing; }

  /** Teleports (scripts setting HumanoidRootPart.Position/CFrame). */
  teleport(p: Vec3) {
    this.pos.set(p.x, p.y, p.z);
    this.body.setTranslation(this.pos, true);
    this.velocity.set(0, 0, 0);
  }

  private notSelf = (col: RAPIER.Collider) => col.handle !== this.collider.handle;

  update(dt: number, move: THREE.Vector3, jump: boolean, faceYaw: number | null) {
    if (this.dead || dt <= 0 || !this.controller) return;
    const hp = this.humanoid.props;
    const speed = Math.max(0, hp.WalkSpeed as number);
    const g = -this.physics.world.gravity.y;
    const p = this.pos;

    // moving platform carry (kinematic/animated parts under the feet)
    if (this.groundPart && this.groundLast && this.groundPart.dm) {
      const now = this.groundPart.props.Position as Vec3;
      p.x += now.x - this.groundLast.x; p.y += Math.max(0, now.y - this.groundLast.y); p.z += now.z - this.groundLast.z;
      this.groundLast = now;
    }

    const wl = this.water(p.x, p.z);
    this.swimming = wl !== null && p.y < wl + 0.5;

    // horizontal velocity eases toward the input direction
    const control = this.grounded || this.swimming ? 1 : 0.35;
    const k = 1 - Math.exp(-(this.grounded ? 25 : 6) * control * dt);
    this.velocity.x += (move.x * speed - this.velocity.x) * k;
    this.velocity.z += (move.z * speed - this.velocity.z) * k;
    this.jumpCooldown -= dt;
    if (this.swimming) {
      const depth = (wl! - 1.2) - p.y;
      const lift = depth > 0 ? 1 + Math.min(1.5, depth * 0.4) : 0.55;
      this.velocity.y += (lift - 1) * g * dt;
      this.velocity.y *= Math.exp(-3 * dt);
      if (jump) this.velocity.y = Math.max(this.velocity.y, 10);
    } else {
      this.velocity.y -= g * dt;
      if (jump && this.grounded && this.jumpCooldown <= 0) {
        this.velocity.y = Math.max(0, hp.JumpPower as number);
        this.jumpCooldown = 0.2;
        this.grounded = false;
        this.onJump();
      }
    }

    // collide-and-slide via Rapier's character controller
    const desired = { x: this.velocity.x * dt, y: this.velocity.y * dt, z: this.velocity.z * dt };
    this.body.setTranslation(p, false);
    this.controller.computeColliderMovement(this.collider, desired, RAPIER.QueryFilterFlags.EXCLUDE_SENSORS, undefined, this.notSelf);
    const mv = this.controller.computedMovement();
    p.set(p.x + mv.x, p.y + mv.y, p.z + mv.z);
    this.body.setNextKinematicTranslation(p);
    const wasGrounded = this.grounded;
    this.grounded = this.controller.computedGrounded();
    if (this.grounded && this.velocity.y < 0) this.velocity.y = 0;
    if (!this.grounded && this.velocity.y > 0 && mv.y < desired.y * 0.5) this.velocity.y = 0; // bumped head
    if (dt > 0 && Math.abs(mv.x) < Math.abs(desired.x) * 0.2) this.velocity.x *= 0.5; // pressing into a wall
    if (dt > 0 && Math.abs(mv.z) < Math.abs(desired.z) * 0.2) this.velocity.z *= 0.5;

    // what are we standing on? (for platform carry + FloorMaterial)
    let floor: Instance | null = null;
    for (let i = 0; i < this.controller.numComputedCollisions(); i++) {
      const col = this.controller.computedCollision(i);
      if (col && col.normal1.y > 0.6 && col.collider) { floor = this.physics.byCollider.get(col.collider.handle) ?? null; break; }
    }
    if (!floor && this.grounded) {
      const hit = this.physics.raycast(v3(p.x, p.y - 2, p.z), v3(0, -1.5, 0), new Set([this.model]), 'Exclude');
      floor = hit?.inst ?? null;
    }
    if (floor !== this.groundPart) {
      this.groundPart = floor && floor.isA('BasePart') ? floor : null;
      this.groundLast = this.groundPart ? (this.groundPart.props.Position as Vec3) : null;
    }
    if (!wasGrounded && !this.grounded && !floor) this.groundPart = null;

    hp.MoveDirection = v3(move.x, 0, move.z);
    hp.FloorMaterial = floor ? (floor.className === 'Terrain' ? 'Grass' : (floor.props.Material as string) ?? 'Plastic') : (this.swimming ? 'Water' : 'Air');

    const horiz = Math.hypot(this.velocity.x, this.velocity.z);
    if (faceYaw !== null) this.yaw = faceYaw;
    else if (move.lengthSq() > 0.01) {
      const target = Math.atan2(-move.x, -move.z);
      const d = Math.atan2(Math.sin(target - this.yaw), Math.cos(target - this.yaw));
      this.yaw += d * Math.min(1, dt * 14);
    }
    this.phase += dt * (this.grounded ? horiz * 0.55 : this.swimming ? 6 : 0);
    const amp = this.grounded ? Math.min(1, horiz / 16) * 0.9 : this.swimming ? 0.6 : 0.25;
    this.swing = this.grounded || this.swimming ? Math.sin(this.phase) * amp : -amp;
    this.detectTouches();
  }

  /** Touched/TouchEnded for everything overlapping the (slightly inflated) capsule, sensors included. */
  detectTouches() {
    const now = new Set<Instance>();
    const shape = new RAPIER.Capsule(CAPSULE_HALF, CAPSULE_RADIUS + 0.08);
    this.physics.world.intersectionsWithShape({ x: this.pos.x, y: this.pos.y + ROOT_TO_CENTER, z: this.pos.z }, { x: 0, y: 0, z: 0, w: 1 }, shape, (c) => {
      const inst = this.physics.byCollider.get(c.handle);
      if (inst && inst !== this.root && inst.className !== 'Terrain') now.add(inst);
      return true;
    }, undefined, undefined, this.collider);
    for (const i of now) if (!this.touching.has(i)) this.physics.trackTouch(this.root, i, true);
    for (const i of this.touching) if (!now.has(i)) this.physics.trackTouch(this.root, i, false);
    this.touching = now;
  }

  /** Call after the physics step to write the position into the instances. */
  postPhysics() { this.writeTransforms(this.swing); }

  writeTransforms(swing: number) {
    const p = this.pos;
    const q = new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(0, 1, 0), this.yaw);
    this.root.setSilent('Position', v3(round(p.x), round(p.y), round(p.z)));
    this.root.setSilent('Orientation', quatToOrientation(q));
    this.root.setSilent('AssemblyLinearVelocity', v3(round(this.velocity.x), round(this.velocity.y), round(this.velocity.z)));
    const rootM = new THREE.Matrix4().compose(p, q, new THREE.Vector3(1, 1, 1));
    const m = new THREE.Matrix4(), pos = new THREE.Vector3(), lq = new THREE.Quaternion(), s = new THREE.Vector3();
    for (const l of this.limbs) {
      m.makeTranslation(l.offset.x + l.pivot.x, l.offset.y + l.pivot.y, l.offset.z + l.pivot.z)
        .multiply(new THREE.Matrix4().makeRotationX(swing * l.swing))
        .multiply(new THREE.Matrix4().makeTranslation(-l.pivot.x, -l.pivot.y, -l.pivot.z));
      m.premultiply(rootM).decompose(pos, lq, s);
      l.inst.setSilent('Position', v3(round(pos.x), round(pos.y), round(pos.z)));
      l.inst.setSilent('Orientation', quatToOrientation(lq));
    }
  }

  setHidden(h: boolean) {
    if (h === this.hidden) return;
    this.hidden = h;
    for (const l of this.limbs) l.inst.setSilent('Visible', !h);
  }

  get headPosition(): THREE.Vector3 { return new THREE.Vector3(this.pos.x, this.pos.y + 1.6, this.pos.z); }

  /** Breaks the character apart into physical pieces. */
  die() {
    if (this.dead) return;
    this.dead = true;
    this.physics.byCollider.delete(this.collider.handle);
    this.physics.world.removeRigidBody(this.body);
    this.setHidden(false);
    for (const t of this.touching) this.physics.trackTouch(this.root, t, false);
    this.touching.clear();
    for (const l of this.limbs) {
      this.physics.kinematic.delete(l.inst);
      if (l.inst.name === 'Face') continue;
      l.inst.setSilent('Anchored', false);
      l.inst.setSilent('CanCollide', true);
      this.physics.add(l.inst);
      const v = this.velocity;
      this.physics.setVelocity(l.inst, v3(v.x + (Math.random() - 0.5) * 12, v.y + Math.random() * 10, v.z + (Math.random() - 0.5) * 12));
    }
  }

  dispose() {
    if (!this.dead) {
      this.physics.byCollider.delete(this.collider.handle);
      this.physics.world.removeRigidBody(this.body);
    }
    if (this.controller) this.physics.world.removeCharacterController(this.controller);
    this.physics.kinematic.delete(this.root);
    for (const l of this.limbs) { this.physics.kinematic.delete(l.inst); this.physics.remove(l.inst); }
  }
}

function round(x: number) { return Math.round(x * 1000) / 1000; }
export { orientationToQuat };

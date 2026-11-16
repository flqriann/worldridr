// GPU instancing for parts: every combination of geometry + material +
// shadow flag is drawn by one InstancedMesh, so thousands of parts cost a
// handful of draw calls. Slots are compacted with swap-remove.

import * as THREE from 'three';
import type { Instance } from '../core/Instance';

const ZERO = new THREE.Matrix4().makeScale(0, 0, 0);

class Batch {
  mesh: THREE.InstancedMesh;
  insts: Instance[] = [];
  constructor(readonly key: string, readonly geometry: THREE.BufferGeometry, readonly material: THREE.Material, readonly castShadow: boolean, capacity: number) {
    this.mesh = this.makeMesh(capacity);
  }
  makeMesh(capacity: number) {
    const m = new THREE.InstancedMesh(this.geometry, this.material, capacity);
    m.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
    m.count = 0;
    m.castShadow = this.castShadow;
    m.receiveShadow = true;
    m.frustumCulled = false; // instances move every frame; per-batch culling would need constant bound updates
    m.userData.batch = this;
    return m;
  }
  get capacity() { return this.mesh.instanceMatrix.count; }
}

export class Batcher {
  private batches = new Map<string, Batch>();
  private slots = new Map<Instance, { b: Batch; i: number }>();
  /** Batches whose matrices changed since the last flush. */
  private dirty = new Set<Batch>();

  constructor(private root: THREE.Object3D) {}

  get meshes(): THREE.InstancedMesh[] { return [...this.batches.values()].map((b) => b.mesh); }
  get batchCount() { return this.batches.size; }
  has(inst: Instance) { return this.slots.has(inst); }

  /** Adds or moves an instance into the batch for (geometry, material, castShadow). */
  set(inst: Instance, geometry: THREE.BufferGeometry, material: THREE.Material, castShadow: boolean, matrix: THREE.Matrix4, visible: boolean) {
    const key = `${geometry.uuid}|${material.uuid}|${castShadow ? 1 : 0}`;
    const slot = this.slots.get(inst);
    if (slot && slot.b.key === key) { this.setMatrix(inst, matrix, visible); return; }
    if (slot) this.remove(inst);
    let b = this.batches.get(key);
    if (!b) {
      b = new Batch(key, geometry, material, castShadow, 16);
      this.batches.set(key, b);
      this.root.add(b.mesh);
    }
    if (b.insts.length >= b.capacity) this.grow(b);
    const i = b.insts.length;
    b.insts.push(inst);
    b.mesh.count = b.insts.length;
    this.slots.set(inst, { b, i });
    this.setMatrix(inst, matrix, visible);
  }

  private grow(b: Batch) {
    const old = b.mesh;
    const next = b.makeMesh(old.instanceMatrix.count * 2);
    next.instanceMatrix.array.set(old.instanceMatrix.array);
    next.count = old.count;
    this.root.add(next);
    old.removeFromParent();
    old.dispose();
    b.mesh = next;
  }

  setMatrix(inst: Instance, matrix: THREE.Matrix4, visible = true) {
    const slot = this.slots.get(inst);
    if (!slot) return;
    slot.b.mesh.setMatrixAt(slot.i, visible ? matrix : ZERO);
    this.dirty.add(slot.b);
  }

  remove(inst: Instance) {
    const slot = this.slots.get(inst);
    if (!slot) return;
    const { b, i } = slot;
    const last = b.insts.length - 1;
    if (i !== last) {
      const moved = b.insts[last];
      b.insts[i] = moved;
      const m = new THREE.Matrix4();
      b.mesh.getMatrixAt(last, m);
      b.mesh.setMatrixAt(i, m);
      this.slots.set(moved, { b, i });
    }
    b.insts.pop();
    b.mesh.count = b.insts.length;
    this.slots.delete(inst);
    this.dirty.add(b);
    if (!b.insts.length) {
      b.mesh.removeFromParent();
      b.mesh.dispose();
      this.batches.delete(b.key);
      this.dirty.delete(b);
    }
  }

  /** Uploads changed matrices (call once per frame before rendering). */
  flush() {
    for (const b of this.dirty) {
      b.mesh.instanceMatrix.needsUpdate = true;
      b.mesh.boundingSphere = null; // raycasts recompute bounds lazily
    }
    this.dirty.clear();
  }

  /** Instance hit by a raycast intersection on one of our meshes. */
  instanceFor(object: THREE.Object3D, instanceId: number | undefined): Instance | undefined {
    const b = object.userData.batch as Batch | undefined;
    return b && instanceId !== undefined ? b.insts[instanceId] : undefined;
  }

  clear() {
    for (const b of this.batches.values()) { b.mesh.removeFromParent(); b.mesh.dispose(); }
    this.batches.clear();
    this.slots.clear();
    this.dirty.clear();
  }
}

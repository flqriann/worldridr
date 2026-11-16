// Shared unit geometries and transform helpers (Position/Orientation/Size <-> three.js).

import * as THREE from 'three';
import type { Instance } from '../core/Instance';
import { v3, type Vec3 } from '../core/types';

const DEG = Math.PI / 180;

function wedgeGeometry(): THREE.BufferGeometry {
  // Unit wedge centered at the origin: full bottom, full back (+Z) face,
  // slope from the bottom-front edge up to the top-back edge.
  const A = [-0.5, -0.5, -0.5], B = [0.5, -0.5, -0.5], C = [0.5, -0.5, 0.5], D = [-0.5, -0.5, 0.5];
  const E = [-0.5, 0.5, 0.5], F = [0.5, 0.5, 0.5];
  const pos: number[] = [], nrm: number[] = [], uv: number[] = [];
  const quad = (p: number[][], n: number[]) => {
    for (const k of [0, 1, 2, 0, 2, 3]) { pos.push(...p[k]); nrm.push(...n); }
    uv.push(0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1);
  };
  const tri = (p: number[][], n: number[]) => { for (const q of p) { pos.push(...q); nrm.push(...n); } uv.push(0, 0, 1, 0, 0, 1); };
  quad([A, B, C, D], [0, -1, 0]);           // bottom (winding chosen to face down)
  quad([D, C, F, E], [0, 0, 1]);            // back
  const s = Math.SQRT1_2;
  quad([B, A, E, F], [0, s, -s]);           // slope
  tri([A, D, E], [-1, 0, 0]);               // left
  tri([B, F, C], [1, 0, 0]);                // right
  const g = new THREE.BufferGeometry();
  g.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
  g.setAttribute('normal', new THREE.Float32BufferAttribute(nrm, 3));
  g.setAttribute('uv', new THREE.Float32BufferAttribute(uv, 2));
  return g;
}

function cylinderX(): THREE.BufferGeometry {
  const g = new THREE.CylinderGeometry(0.5, 0.5, 1, 32, 1);
  g.rotateZ(Math.PI / 2);
  return g;
}

export const GEOMETRY = {
  Block: new THREE.BoxGeometry(1, 1, 1),
  Ball: new THREE.SphereGeometry(0.5, 32, 18),
  Cylinder: cylinderX(),
  Wedge: wedgeGeometry(),
};

/** Edge outline geometries matching the shapes (unit size). */
export const OUTLINES = {
  Block: new THREE.EdgesGeometry(GEOMETRY.Block),
  Ball: new THREE.EdgesGeometry(new THREE.BoxGeometry(1, 1, 1)),
  Cylinder: new THREE.EdgesGeometry(new THREE.BoxGeometry(1, 1, 1)),
  Wedge: new THREE.EdgesGeometry(GEOMETRY.Wedge, 1),
};

export type ShapeKind = keyof typeof GEOMETRY;

export function shapeOf(inst: Instance): ShapeKind {
  if (inst.className === 'WedgePart') return 'Wedge';
  const s = inst.props.Shape as string | undefined;
  if (s === 'Ball') return 'Ball';
  if (s === 'Cylinder') return 'Cylinder';
  return 'Block';
}

/** Visual/physical extents for a part, accounting for Ball/Cylinder constraints. */
export function effectiveSize(inst: Instance): Vec3 {
  const s = inst.props.Size as Vec3;
  const shape = shapeOf(inst);
  if (shape === 'Ball') { const d = Math.min(s.x, s.y, s.z); return v3(d, d, d); }
  if (shape === 'Cylinder') { const d = Math.min(s.y, s.z); return v3(s.x, d, d); }
  return s;
}

const _e = new THREE.Euler();
export function orientationToQuat(o: Vec3, out = new THREE.Quaternion()): THREE.Quaternion {
  _e.set(o.x * DEG, o.y * DEG, o.z * DEG, 'YXZ');
  return out.setFromEuler(_e);
}

export function quatToOrientation(q: THREE.Quaternion): Vec3 {
  _e.setFromQuaternion(q, 'YXZ');
  const r = (x: number) => { const d = Math.round((x / DEG) * 1000) / 1000; return Object.is(d, -0) ? 0 : d; };
  return v3(r(_e.x), r(_e.y), r(_e.z));
}

export const toVec3 = (v: THREE.Vector3): Vec3 => v3(v.x, v.y, v.z);
export const toThree = (v: Vec3, out = new THREE.Vector3()) => out.set(v.x, v.y, v.z);

/** World matrix of a part/camera from its Position + Orientation (no scale). */
export function instanceMatrix(inst: Instance, out = new THREE.Matrix4()): THREE.Matrix4 {
  const p = inst.props.Position as Vec3;
  const o = inst.props.Orientation as Vec3;
  return out.compose(toThree(p), orientationToQuat(o), new THREE.Vector3(1, 1, 1));
}

export function isTransformable(inst: Instance) {
  return inst.isA('BasePart') || inst.className === 'Camera';
}

/** All transformable instances in a selection (models expand to their parts). */
export function collectTransformables(list: readonly Instance[]): Instance[] {
  const out = new Set<Instance>();
  for (const i of list) {
    if (isTransformable(i)) out.add(i);
    else if (i.className === 'Model' || i.className === 'Folder') for (const d of i.getDescendants()) if (d.isA('BasePart')) out.add(d);
  }
  return [...out];
}

/** World-space axis-aligned bounding box of parts. */
export function boundsOf(parts: Instance[]): THREE.Box3 {
  const box = new THREE.Box3();
  const m = new THREE.Matrix4();
  const tmp = new THREE.Box3();
  for (const p of parts) {
    if (!p.isA('BasePart')) { box.expandByPoint(toThree(p.props.Position as Vec3)); continue; }
    const s = effectiveSize(p);
    tmp.set(new THREE.Vector3(-s.x / 2, -s.y / 2, -s.z / 2), new THREE.Vector3(s.x / 2, s.y / 2, s.z / 2));
    tmp.applyMatrix4(instanceMatrix(p, m));
    box.union(tmp);
  }
  return box;
}

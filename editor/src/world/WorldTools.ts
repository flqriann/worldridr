// Interactive road editing in the viewport:
//  - Road tool: click the ground to add points; Enter / double-click finishes, Backspace removes the last point, Esc cancels.
//    Clicking near an existing road end snaps to it (junction).
//  - With one Road selected: drag the handles to move points, Alt+click a handle removes it,
//    Ctrl+click on the road inserts a point.

import * as THREE from 'three';
import { Instance } from '../core/Instance';
import { v3, type Vec3 } from '../core/types';
import type { Viewport } from '../render/Viewport';
import { parseNumberList } from './shapes';
import { orientationToQuat } from '../render/geometry';

type WP = [number, number, number]; // world x, z, lift

const HANDLE = 0xffffff;
const HANDLE_HOT = 0xffd24a;

/** Road points as world coordinates. */
export function roadWorldPoints(inst: Instance): WP[] {
  const pts = parseNumberList(inst.props.Points as string);
  const q = orientationToQuat(inst.props.Orientation as Vec3);
  const pos = inst.props.Position as Vec3;
  const out: WP[] = [];
  const v = new THREE.Vector3();
  for (let i = 0; i + 2 < pts.length; i += 3) {
    v.set(pts[i], 0, pts[i + 1]).applyQuaternion(q);
    out.push([v.x + pos.x, v.z + pos.z, pts[i + 2]]);
  }
  return out;
}

/** Writes world points back: part origin = bounding-box center, orientation reset. */
export function roadSetWorldPoints(inst: Instance, world: WP[], heightAt: (x: number, z: number) => number, silent = false) {
  let x0 = Infinity, x1 = -Infinity, z0 = Infinity, z1 = -Infinity;
  for (const p of world) { x0 = Math.min(x0, p[0]); x1 = Math.max(x1, p[0]); z0 = Math.min(z0, p[1]); z1 = Math.max(z1, p[1]); }
  const cx = (x0 + x1) / 2, cz = (z0 + z1) / 2;
  const flat: number[] = [];
  for (const p of world) flat.push(Math.round((p[0] - cx) * 1000) / 1000, Math.round((p[1] - cz) * 1000) / 1000, Math.round(p[2] * 1000) / 1000);
  const set = (k: string, v: unknown) => (silent ? inst.setSilent(k, v) : inst.set(k, v));
  set('Orientation', v3(0, 0, 0));
  set('Position', v3(Math.round(cx * 1000) / 1000, Math.round(heightAt(cx, cz) * 1000) / 1000, Math.round(cz * 1000) / 1000));
  set('Size', v3(Math.max(2, x1 - x0), 0.3, Math.max(2, z1 - z0)));
  set('Points', JSON.stringify(flat));
}

export class WorldTools {
  private handles = new THREE.Group();
  private handleMeshes: THREE.Mesh[] = [];
  private shown: Instance | null = null;
  private shownSig = '';
  private cursor: THREE.Mesh;
  private drawing: Instance | null = null;
  private drag: null | { road: Instance; index: number; before: string; beforePos: Vec3; beforeSize: Vec3; beforeOri: Vec3 } = null;
  private lastClick = 0;
  private sphere = new THREE.SphereGeometry(1, 12, 8);
  private ray = new THREE.Raycaster();
  /** Called to tell the UI something short ("Road: 4 points"). */
  onStatus: ((text: string) => void) | null = null;

  constructor(private vp: Viewport) {
    this.handles.renderOrder = 998;
    vp.scene.add(this.handles);
    this.cursor = new THREE.Mesh(new THREE.RingGeometry(0.7, 1, 32), new THREE.MeshBasicMaterial({ color: 0xffd24a, transparent: true, opacity: 0.9, depthTest: false, side: THREE.DoubleSide }));
    this.cursor.rotation.x = -Math.PI / 2;
    this.cursor.renderOrder = 999;
    this.cursor.visible = false;
    vp.scene.add(this.cursor);
    window.addEventListener('keydown', (e) => this.onKey(e));
  }

  get dragging() { return !!this.drag; }
  private get doc() { return this.vp.doc; }
  private heightAt = (x: number, z: number) => this.vp.dm?.terrainInstance?.terrain?.heightAt(x, z) ?? 0;

  private setRay(e: { clientX: number; clientY: number }) {
    const r = this.vp.canvas.getBoundingClientRect();
    const p = new THREE.Vector2(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
    this.ray.setFromCamera(p, this.vp.camera);
  }

  /** Ground point under the cursor (terrain, objects, else the plane at the given height). */
  private groundPoint(e: { clientX: number; clientY: number }, fallbackY = 0): THREE.Vector3 | null {
    const hit = this.vp.pick(e.clientX, e.clientY, { includeTerrain: true, includeLocked: true });
    if (hit) return hit.point.clone();
    this.setRay(e);
    const out = new THREE.Vector3();
    return this.ray.ray.intersectPlane(new THREE.Plane(new THREE.Vector3(0, 1, 0), -fallbackY), out) ? out : null;
  }

  private roadsFolder(): Instance {
    const ws = this.vp.dm!.workspace;
    let f = ws.children.find((c) => c.className === 'Folder' && c.name === 'Roads');
    if (!f) { f = new Instance('Folder'); f.props.Name = 'Roads'; f.setParent(ws); }
    return f;
  }

  private snapPoint(x: number, z: number): [number, number] {
    let best = 4 * 4, bx = x, bz = z;
    this.vp.dm?.workspace.walk((i) => {
      if (i.className !== 'Road' || i === this.drawing) return;
      const w = roadWorldPoints(i);
      for (const p of [w[0], w[w.length - 1]]) {
        if (!p) continue;
        const d = (p[0] - x) ** 2 + (p[1] - z) ** 2;
        if (d < best) { best = d; bx = p[0]; bz = p[1]; }
      }
    });
    return [bx, bz];
  }

  // ---- pointer hooks (return true when the event was consumed) -------------------------

  pointerDown(e: PointerEvent): boolean {
    if (e.button !== 0 || !this.doc) return false;
    if (this.vp.tool === 'road') {
      const g = this.groundPoint(e);
      if (!g) return true;
      const now = performance.now();
      const dbl = now - this.lastClick < 350;
      this.lastClick = now;
      if (dbl && this.drawing) { this.finish(); return true; }
      const [x, z] = this.snapPoint(g.x, g.z);
      this.addPoint(x, z);
      return true;
    }
    // handles of the selected road
    const road = this.selectedRoad();
    if (!road) return false;
    this.setRay(e);
    if (e.ctrlKey || e.metaKey) {
      const g = this.groundPoint(e);
      const hit = this.vp.pick(e.clientX, e.clientY, { includeLocked: true });
      if (g && hit?.inst === road) { this.insertPoint(road, g.x, g.z); return true; }
    }
    const h = this.ray.intersectObjects(this.handleMeshes, false)[0];
    if (!h) return false;
    const index = h.object.userData.index as number;
    if (e.altKey) { this.removePoint(road, index); return true; }
    this.drag = {
      road, index, before: road.props.Points as string, beforePos: road.props.Position as Vec3, beforeSize: road.props.Size as Vec3, beforeOri: road.props.Orientation as Vec3,
    };
    return true;
  }

  pointerMove(e: PointerEvent): boolean {
    if (this.vp.tool === 'road') {
      const g = this.groundPoint(e);
      this.cursor.visible = !!g;
      if (g) {
        const [x, z] = this.snapPoint(g.x, g.z);
        this.cursor.position.set(x, this.heightAt(x, z) + 0.4, z);
        const s = Math.max(1.2, this.vp.camera.position.distanceTo(this.cursor.position) * 0.012);
        this.cursor.scale.setScalar(s);
      }
      return false;
    }
    this.cursor.visible = false;
    if (!this.drag) return false;
    const g = this.groundPoint(e, 0);
    if (!g) return true;
    const w = roadWorldPointsFrom(this.drag);
    const [x, z] = e.shiftKey ? [g.x, g.z] : this.snapPoint(g.x, g.z);
    w[this.drag.index] = [x, z, w[this.drag.index][2]];
    roadSetWorldPoints(this.drag.road, w, this.heightAt, true);
    return true;
  }

  pointerUp(e: PointerEvent): boolean {
    if (!this.drag || e.button !== 0) return false;
    const d = this.drag;
    this.drag = null;
    const after = { Points: d.road.props.Points, Position: d.road.props.Position, Size: d.road.props.Size, Orientation: d.road.props.Orientation };
    d.road.setSilent('Points', d.before); d.road.setSilent('Position', d.beforePos); d.road.setSilent('Size', d.beforeSize); d.road.setSilent('Orientation', d.beforeOri);
    if (this.doc) {
      this.doc.transact('Move road point', () => { for (const [k, v] of Object.entries(after)) d.road.set(k, v); });
    }
    return true;
  }

  private onKey(e: KeyboardEvent) {
    if (this.vp.tool !== 'road' || !this.drawing) return;
    const t = e.target as HTMLElement | null;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA')) return;
    if (e.key === 'Enter') { e.preventDefault(); this.finish(); }
    else if (e.key === 'Escape') { e.preventDefault(); this.finish(); }
    else if (e.key === 'Backspace') {
      e.preventDefault();
      const w = roadWorldPoints(this.drawing);
      w.pop();
      if (w.length < 2) { this.drawing.destroy(); this.drawing = null; this.cursor.visible = true; return; }
      this.doc?.transact('Remove road point', () => roadSetWorldPoints(this.drawing!, w, this.heightAt));
    }
  }

  // ---- editing operations ------------------------------------------------------------

  private addPoint(x: number, z: number) {
    const doc = this.doc;
    if (!doc) return;
    doc.transact('Draw road', () => {
      if (!this.drawing) {
        const r = new Instance('Road');
        r.props.Name = 'Road';
        r.props.RoadType = this.vp.roadType;
        r.setParent(this.roadsFolder());
        roadSetWorldPoints(r, [[x, z, 0], [x + 0.01, z + 0.01, 0]], this.heightAt);
        this.drawing = r;
        doc.selection.set([r]);
        this.onStatus?.('Road: click to add points, Enter or double-click to finish');
        return;
      }
      const w = roadWorldPoints(this.drawing).filter((p, i, a) => !(i === a.length - 1 && a.length === 2 && Math.hypot(a[1][0] - a[0][0], a[1][1] - a[0][1]) < 0.1));
      w.push([x, z, 0]);
      roadSetWorldPoints(this.drawing, w, this.heightAt);
    });
  }

  private finish() {
    const r = this.drawing;
    this.drawing = null;
    if (r && roadWorldPoints(r).length < 2) r.destroy();
    this.onStatus?.('');
  }

  private insertPoint(road: Instance, x: number, z: number) {
    const w = roadWorldPoints(road);
    let bi = 0, bd = Infinity;
    for (let i = 0; i < w.length - 1; i++) {
      const d = distSeg(x, z, w[i], w[i + 1]);
      if (d < bd) { bd = d; bi = i; }
    }
    w.splice(bi + 1, 0, [x, z, 0]);
    this.doc?.transact('Insert road point', () => roadSetWorldPoints(road, w, this.heightAt));
  }

  private removePoint(road: Instance, index: number) {
    const w = roadWorldPoints(road);
    if (w.length <= 2) return;
    w.splice(index, 1);
    this.doc?.transact('Remove road point', () => roadSetWorldPoints(road, w, this.heightAt));
  }

  private selectedRoad(): Instance | null {
    const d = this.doc;
    if (!d || this.vp.mode !== 'edit' || d.selection.size !== 1) return null;
    const s = d.selection.primary;
    return s && s.className === 'Road' ? s : null;
  }

  /** Per frame: keep the handle spheres in sync with the selected road. */
  update() {
    const road = this.vp.tool === 'terrain' ? null : this.selectedRoad();
    if (!road) {
      if (this.shown) { this.clearHandles(); }
      return;
    }
    const sig = `${road.id}|${road.props.Points}|${(road.props.Position as Vec3).x},${(road.props.Position as Vec3).z}`;
    const w = roadWorldPoints(road);
    // rebuild when the road or its terrain changed (cheap: a handful of spheres)
    if (this.shown !== road || this.shownSig !== sig || this.handleMeshes.length !== w.length) {
      this.clearHandles();
      this.shown = road;
      this.shownSig = sig;
      w.forEach((p, i) => {
        const m = new THREE.Mesh(this.sphere, new THREE.MeshBasicMaterial({ color: i === 0 || i === w.length - 1 ? HANDLE_HOT : HANDLE, depthTest: false }));
        m.userData.index = i;
        m.renderOrder = 998;
        this.handles.add(m);
        this.handleMeshes.push(m);
      });
    }
    const cam = this.vp.camera.position;
    w.forEach((p, i) => {
      const m = this.handleMeshes[i];
      if (!m) return;
      m.position.set(p[0], this.heightAt(p[0], p[1]) + p[2] + 0.6, p[1]);
      m.scale.setScalar(Math.max(0.7, cam.distanceTo(m.position) * 0.012));
    });
  }

  private clearHandles() {
    for (const m of this.handleMeshes) { m.removeFromParent(); (m.material as THREE.Material).dispose(); }
    this.handleMeshes = [];
    this.shown = null;
    this.shownSig = '';
  }

  /** The tool changed: finish a road in progress. */
  toolChanged() { if (this.vp.tool !== 'road') { this.finish(); this.cursor.visible = false; } }
}

function roadWorldPointsFrom(d: { road: Instance; before: string; beforePos: Vec3; beforeOri: Vec3 }): WP[] {
  const pts = parseNumberList(d.before);
  const q = orientationToQuat(d.beforeOri);
  const out: WP[] = [];
  const v = new THREE.Vector3();
  for (let i = 0; i + 2 < pts.length; i += 3) {
    v.set(pts[i], 0, pts[i + 1]).applyQuaternion(q);
    out.push([v.x + d.beforePos.x, v.z + d.beforePos.z, pts[i + 2]]);
  }
  return out;
}

function distSeg(x: number, z: number, a: WP, b: WP): number {
  const dx = b[0] - a[0], dz = b[1] - a[1];
  const l2 = dx * dx + dz * dz || 1;
  const t = Math.max(0, Math.min(1, ((x - a[0]) * dx + (z - a[1]) * dz) / l2));
  return Math.hypot(x - (a[0] + dx * t), z - (a[1] + dz * t));
}

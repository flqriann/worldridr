// The 3D viewport: renders a DataModel, provides the editor camera, picking,
// selection outlines, transform gizmos, scale handles, terrain brushing and
// the GUI overlay. In play mode the runtime takes over camera/input via hooks.

import * as THREE from 'three';
import { TransformControls } from 'three/examples/jsm/controls/TransformControls.js';
import type { DataModel, Instance } from '../core/Instance';
import { Signal } from '../core/Signal';
import { v3, type Vec3 } from '../core/types';
import type { Document } from '../editor/Document';
import type { AssetStore } from '../assets/AssetStore';
import { MaterialCache } from './materials';
import { Environment } from './Environment';
import { GEOMETRY, OUTLINES, boundsOf, collectTransformables, effectiveSize, instanceMatrix, isTransformable, orientationToQuat, quatToOrientation, shapeOf, toThree, toVec3 } from './geometry';
import { TerrainView } from './TerrainView';
import { ParticleSystem } from './Particles';
import { GuiRenderer } from './GuiRenderer';
import { loadModelAsset } from './models';
import { Batcher } from './Batcher';
import { WorldLayer, isLayerClass } from '../world/WorldLayer';
import { WorldTools } from '../world/WorldTools';
import { applyBrush, type BrushOptions } from '../terrain/TerrainTools';
import { unionRegion, type Region, type TerrainSnapshot } from '../terrain/TerrainData';

export type Tool = 'select' | 'move' | 'rotate' | 'scale' | 'terrain' | 'road';

interface PartView { kind: 'part'; group: THREE.Group; body: THREE.Group; mesh: THREE.Object3D; decal?: THREE.Mesh; meshKey: string }
interface LightView { kind: 'light'; light: THREE.PointLight | THREE.SpotLight; target?: THREE.Object3D }
interface EmitterView { kind: 'emitter'; ps: ParticleSystem }
interface CameraView { kind: 'camera'; group: THREE.Group }
interface TerrainWrap { kind: 'terrain'; tv: TerrainView }
interface DecalView { kind: 'decal'; mesh: THREE.Mesh; material: THREE.MeshStandardMaterial }
type View = PartView | LightView | EmitterView | CameraView | TerrainWrap | DecalView;

const SELECT_COLOR = 0x1aa3ff;
const HOVER_COLOR = 0x9fd8ff;
const DEG = Math.PI / 180;

let spawnDecalTex: THREE.Texture | null = null;
function spawnTexture() {
  if (spawnDecalTex) return spawnDecalTex;
  const c = document.createElement('canvas');
  c.width = c.height = 128;
  const g = c.getContext('2d')!;
  g.strokeStyle = 'rgba(255,255,255,0.85)'; g.lineWidth = 8;
  g.beginPath(); g.arc(64, 64, 50, 0, 7); g.stroke();
  g.fillStyle = 'rgba(255,255,255,0.85)';
  g.beginPath(); g.moveTo(64, 26); g.lineTo(92, 74); g.lineTo(72, 74); g.lineTo(72, 100); g.lineTo(56, 100); g.lineTo(56, 74); g.lineTo(36, 74); g.closePath(); g.fill();
  spawnDecalTex = new THREE.CanvasTexture(c);
  spawnDecalTex.colorSpace = THREE.SRGBColorSpace;
  return spawnDecalTex;
}

export interface ViewportInput {
  /** Edit-mode callbacks */
  onContextMenu?: (x: number, y: number, target: Instance | null) => void;
  onMessage?: (level: 'info' | 'warn' | 'error', msg: string) => void;
  /** A Toolbox card was dropped onto the 3D view at a world surface point. */
  onToolboxDrop?: (itemId: string, point: Vec3) => void;
  /** Files from the operating system dropped onto the 3D view. */
  onFilesDrop?: (files: File[], point: Vec3 | undefined) => void;
}

export class Viewport {
  readonly renderer: THREE.WebGLRenderer;
  readonly scene = new THREE.Scene();
  readonly camera = new THREE.PerspectiveCamera(70, 1, 0.1, 6000);
  readonly materials: MaterialCache;
  readonly env: Environment;
  readonly gui: GuiRenderer;
  readonly container: HTMLElement;
  readonly canvas: HTMLCanvasElement;
  private root = new THREE.Group();
  private batcher: Batcher;
  readonly world: WorldLayer;
  readonly worldTools: WorldTools;
  private views = new Map<Instance, View>();
  private pickables = new Set<THREE.Object3D>();
  private outlines = new Map<Instance, THREE.Object3D>();
  private hoverOutline: THREE.LineSegments;
  private hovered: Instance | null = null;
  private grid: THREE.GridHelper | null = null;
  private brushRing: THREE.Mesh;
  private tc: TransformControls;
  private pivot = new THREE.Object3D();
  private scaleHandles = new THREE.Group();
  private marquee: HTMLDivElement;
  private unsub: (() => void)[] = [];
  private raf = 0;
  private lastTime = performance.now();
  private resizeObs: ResizeObserver;

  dm: DataModel | null = null;
  doc: Document | null = null;
  assets: AssetStore | null = null;
  mode: 'edit' | 'play' = 'edit';
  tool: Tool = 'select';
  space: 'world' | 'local' = 'world';
  /** Road type used by the Road tool for new roads. */
  roadType = 'Stadtstraße';
  showGrid = true;
  terrainBrush: BrushOptions = { tool: 'raise', radius: 12, strength: 0.5, material: 1, flattenHeight: 0 };
  input: ViewportInput = {};

  // editor camera
  camPos = new THREE.Vector3(0, 16, 32);
  yaw = 0;
  pitch = -0.4;
  private keys = new Set<string>();
  private drag: null | { kind: 'look' | 'pan' | 'left'; x: number; y: number; moved: boolean; button: number } = null;
  private moveDrag: null | { parts: Instance[]; starts: Vec3[]; orients: Vec3[]; plane: THREE.Plane; startPoint: THREE.Vector3; bottom: number; changed: boolean; center: THREE.Vector3; turn: THREE.Quaternion; delta: THREE.Vector3; lastEvent: PointerEvent } = null;
  private marqueeStart: { x: number; y: number } | null = null;
  private gizmoStart: { pivot: THREE.Matrix4; parts: Instance[]; mats: THREE.Matrix4[] } | null = null;
  private scaleDrag: null | { inst: Instance; axis: THREE.Vector3; local: number; sign: number; startSize: Vec3; startPos: Vec3; t0: number; origin: THREE.Vector3 } = null;
  private brushStroke: { before: TerrainSnapshot; inst: Instance; region: Region | null } | null = null;
  private lastPointer: { clientX: number; clientY: number } | null = null;
  private brushHit: THREE.Vector3 | null = null;
  private pointer = new THREE.Vector2();
  private pointerInside = false;
  private raycaster = new THREE.Raycaster();

  /** Fired every frame before rendering (dt in seconds). The runtime hooks in here. */
  readonly beforeRender = new Signal<[number]>();
  readonly stats = { fps: 0, drawCalls: 0, triangles: 0, frameMs: 0 };
  private fpsAcc = { t: 0, n: 0 };
  /** In play mode the runtime takes the camera; set to true to skip the editor camera update. */
  externalCamera = false;
  /** Called with pointer events while in play mode. */
  playPointer: ((e: PointerEvent | WheelEvent, kind: 'down' | 'move' | 'up' | 'wheel') => void) | null = null;

  constructor(container: HTMLElement) {
    this.container = container;
    this.renderer = new THREE.WebGLRenderer({ antialias: true, powerPreference: 'high-performance', preserveDrawingBuffer: false });
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    this.renderer.shadowMap.enabled = true;
    this.renderer.shadowMap.type = THREE.PCFSoftShadowMap;
    this.renderer.toneMapping = THREE.ACESFilmicToneMapping;
    this.renderer.outputColorSpace = THREE.SRGBColorSpace;
    this.canvas = this.renderer.domElement;
    this.canvas.className = 'viewport-canvas';
    this.canvas.tabIndex = 0;
    container.appendChild(this.canvas);
    this.materials = new MaterialCache(() => this.assets);
    this.env = new Environment(this.scene, this.renderer);
    this.scene.add(this.root);
    this.batcher = new Batcher(this.root);
    this.world = new WorldLayer(this.root);
    this.world.heightAt = (x, z) => this.dm?.terrainInstance?.terrain?.heightAt(x, z) ?? 0;
    this.worldTools = new WorldTools(this);
    this.gui = new GuiRenderer(container, () => this.assets);

    const hoverMat = new THREE.LineBasicMaterial({ color: HOVER_COLOR, transparent: true, opacity: 0.8, depthTest: true });
    this.hoverOutline = new THREE.LineSegments(OUTLINES.Block, hoverMat);
    this.hoverOutline.visible = false;
    this.scene.add(this.hoverOutline);

    this.brushRing = new THREE.Mesh(new THREE.RingGeometry(0.94, 1, 64), new THREE.MeshBasicMaterial({ color: 0x4fc3ff, transparent: true, opacity: 0.85, depthTest: false, side: THREE.DoubleSide }));
    this.brushRing.rotation.x = -Math.PI / 2;
    this.brushRing.renderOrder = 999;
    this.brushRing.visible = false;
    this.scene.add(this.brushRing);

    this.tc = new TransformControls(this.camera, this.canvas);
    this.tc.setSize(0.9);
    this.scene.add(this.tc.getHelper());
    this.scene.add(this.pivot);
    this.tc.addEventListener('mouseDown', () => this.gizmoBegin());
    this.tc.addEventListener('objectChange', () => this.gizmoChange());
    this.tc.addEventListener('mouseUp', () => this.gizmoEnd());
    this.buildScaleHandles();

    this.marquee = document.createElement('div');
    this.marquee.className = 'marquee';
    container.appendChild(this.marquee);

    this.bindInput();
    this.resizeObs = new ResizeObserver(() => this.resize());
    this.resizeObs.observe(container);
    this.resize();
    this.materials.onTextureLoaded = () => {};
    this.loop();
    document.addEventListener('visibilitychange', this.onVisibility);
    if (document.hidden) this.onVisibility();
  }

  // ---- binding ------------------------------------------------------------------

  /** Shows a DataModel. `doc` is set in edit mode (selection + undo), null in play mode views. */
  setDataModel(dm: DataModel | null, doc: Document | null, assets: AssetStore | null, mode: 'edit' | 'play') {
    this.clearViews();
    this.unsub.forEach((u) => u());
    this.unsub = [];
    this.dm = dm;
    this.doc = doc;
    this.assets = assets;
    this.mode = mode;
    this.externalCamera = false;
    this.playPointer = null;
    this.tc.detach();
    if (!dm) { this.gui.setContainer(null, null, false); return; }
    this.env.bind(dm);
    dm.walk((i) => { this.createView(i); });
    this.unsub.push(
      dm.added.connect((i) => this.createView(i)),
      dm.removed.connect((i) => this.destroyView(i)),
      dm.reparented.connect((i) => { i.walk((d) => { this.destroyView(d); }); i.walk((d) => { this.createView(d); }); this.refreshSelectionVisuals(); }),
      dm.changed.connect((i, prop) => this.onChanged(i, prop)),
      dm.transformsChanged.connect((list) => { for (const i of list) { const v = this.views.get(i); if (v?.kind === 'part') this.updatePartTransform(i, v); } }),
      dm.terrainChanged.connect((i, region) => {
        const v = this.views.get(i);
        if (v?.kind !== 'terrain') return;
        this.world.markRoadsDirty();
        const waterTool = this.terrainBrush.tool === 'addWater' || this.terrainBrush.tool === 'removeWater' || this.terrainBrush.tool === 'erase';
        v.tv.rebuild(region, !region || waterTool || !this.brushStroke);
      }),
    );
    if (doc) {
      this.unsub.push(doc.selection.changed.connect(() => this.refreshSelectionVisuals()));
      if (doc.camera) {
        this.camPos.fromArray(doc.camera.position);
        this.yaw = doc.camera.yaw;
        this.pitch = doc.camera.pitch;
      }
    }
    if (mode === 'edit') this.gui.setContainer(dm, dm.getService('StarterGui') ?? null, false);
    this.updateGrid();
    this.refreshSelectionVisuals();
  }

  saveCameraTo(doc: Document) {
    doc.camera = { position: [this.camPos.x, this.camPos.y, this.camPos.z], yaw: this.yaw, pitch: this.pitch };
  }

  private inWorkspace(i: Instance) {
    const ws = this.dm?.workspace;
    return !!ws && i.isDescendantOf(ws);
  }

  private createView(i: Instance) {
    if (this.views.has(i) || !this.dm) return;
    if (!this.inWorkspace(i) && !(i.parent?.isA('BasePart'))) return;
    if (i.isA('BasePart')) {
      if (!this.inWorkspace(i)) return;
      const group = new THREE.Group();
      const body = new THREE.Group();
      group.add(body);
      group.userData.inst = i;
      const view: PartView = { kind: 'part', group, body, mesh: new THREE.Object3D(), meshKey: '' };
      this.views.set(i, view);
      this.root.add(group);
      this.buildPartMesh(i, view);
      this.updatePartTransform(i, view);
      // attachments created before this part (e.g. during reparent) are picked up by createView of children
    } else if (i.className === 'Terrain') {
      if (!i.terrain) return;
      const tv = new TerrainView(i);
      this.views.set(i, { kind: 'terrain', tv });
      this.root.add(tv.group);
    } else if (i.className === 'Camera') {
      if (!this.inWorkspace(i)) return;
      const group = new THREE.Group();
      const camMat = new THREE.MeshBasicMaterial({ color: 0x8899aa, wireframe: true, transparent: true, opacity: 0.45 });
      const body = new THREE.Mesh(new THREE.BoxGeometry(0.8, 0.6, 0.9), camMat);
      const lens = new THREE.Mesh(new THREE.ConeGeometry(0.4, 0.6, 4, 1, true), camMat);
      lens.rotation.x = Math.PI / 2; lens.position.z = -0.75;
      body.userData.inst = i; lens.userData.inst = i;
      group.add(body, lens);
      group.visible = this.mode === 'edit' && !!this.doc?.selection.has(i);
      this.pickables.add(body);
      this.views.set(i, { kind: 'camera', group });
      this.root.add(group);
      this.applyCameraTransform(i);
    } else if (i.isA('Light')) {
      const pv = i.parent ? this.views.get(i.parent) : undefined;
      if (!pv || pv.kind !== 'part') return;
      let light: THREE.PointLight | THREE.SpotLight;
      let target: THREE.Object3D | undefined;
      if (i.className === 'SpotLight') {
        const s = new THREE.SpotLight();
        target = new THREE.Object3D();
        s.target = target;
        pv.group.add(target);
        light = s;
      } else light = new THREE.PointLight();
      pv.group.add(light);
      this.views.set(i, { kind: 'light', light, target });
      this.updateLight(i);
    } else if (i.className === 'Decal') {
      const pv = i.parent ? this.views.get(i.parent) : undefined;
      if (!pv || pv.kind !== 'part') return;
      const material = new THREE.MeshStandardMaterial({ transparent: true, depthWrite: false, polygonOffset: true, polygonOffsetFactor: -4, roughness: 0.8 });
      const mesh = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), material);
      mesh.userData.inst = i.parent;
      pv.group.add(mesh);
      this.views.set(i, { kind: 'decal', mesh, material });
      this.updateDecal(i);
    } else if (i.className === 'ParticleEmitter') {
      const pv = i.parent ? this.views.get(i.parent) : undefined;
      if (!pv || pv.kind !== 'part') return;
      const ps = new ParticleSystem(i);
      ps.setViewportHeight(this.container.clientHeight || 600);
      this.root.add(ps.points);
      this.views.set(i, { kind: 'emitter', ps });
    }
  }

  private destroyView(i: Instance) {
    const v = this.views.get(i);
    if (!v) return;
    this.views.delete(i);
    switch (v.kind) {
      case 'part':
        this.batcher.remove(i);
        this.world.remove(i);
        v.group.removeFromParent();
        v.group.traverse((o) => this.pickables.delete(o));
        // re-home attachment views (lights) are children of the group and vanish with it
        for (const c of i.children) if (this.views.get(c)?.kind === 'light') this.views.delete(c);
        break;
      case 'terrain': v.tv.group.removeFromParent(); v.tv.dispose(); break;
      case 'camera': v.group.removeFromParent(); v.group.traverse((o) => this.pickables.delete(o)); break;
      case 'light': v.light.removeFromParent(); v.target?.removeFromParent(); v.light.dispose(); break;
      case 'emitter': v.ps.points.removeFromParent(); v.ps.dispose(); break;
      case 'decal': v.mesh.removeFromParent(); v.mesh.geometry.dispose(); v.material.dispose(); break;
    }
    const o = this.outlines.get(i);
    if (o) { o.removeFromParent(); this.outlines.delete(i); }
    if (this.hovered === i) this.setHover(null);
  }

  private clearViews() {
    for (const i of [...this.views.keys()]) this.destroyView(i);
    this.batcher.clear();
    this.world.clear();
    this.pickables.clear();
    this.outlines.clear();
  }

  private buildPartMesh(i: Instance, v: PartView) {
    const shape = shapeOf(i);
    const p = i.props;
    const key = i.className === 'MeshPart' ? `mesh:${p.MeshId}:${p.UseModelMaterials}` : `shape:${shape}`;
    if (key === v.meshKey) { this.updatePartLook(i, v); return; }
    v.meshKey = key;
    v.mesh.removeFromParent();
    v.mesh.traverse((o) => this.pickables.delete(o));
    if (i.className === 'MeshPart' && p.MeshId) {
      const placeholder = new THREE.Mesh(GEOMETRY.Block, new THREE.MeshBasicMaterial({ color: 0x777777, wireframe: true }));
      placeholder.userData.inst = i;
      v.mesh = placeholder;
      v.body.add(placeholder);
      this.pickables.add(placeholder);
      const asset = this.assets?.get(p.MeshId as string);
      if (!asset) { this.input.onMessage?.('warn', `${i.getFullName()}: model asset "${p.MeshId}" not found`); return; }
      loadModelAsset(asset).then((m) => {
        if (this.views.get(i) !== v || v.meshKey !== key) return;
        const clone = m.scene.clone(true);
        clone.traverse((o) => { o.userData.inst = i; if ((o as THREE.Mesh).isMesh) this.pickables.add(o); });
        placeholder.removeFromParent();
        this.pickables.delete(placeholder);
        v.mesh = clone;
        v.body.add(clone);
        this.updatePartLook(i, v);
      }).catch((e) => this.input.onMessage?.('error', (e as Error).message));
      return;
    }
    // plain parts are drawn through the instancing batcher (see updatePartLook)
    v.mesh = new THREE.Object3D();
    v.body.add(v.mesh);
    if (i.className === 'SpawnLocation' && !v.decal) {
      v.decal = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), new THREE.MeshBasicMaterial({ map: spawnTexture(), transparent: true, depthWrite: false, polygonOffset: true, polygonOffsetFactor: -2 }));
      v.decal.rotation.x = -Math.PI / 2;
      v.group.add(v.decal);
    }
    this.updatePartLook(i, v);
  }

  private updatePartLook(i: Instance, v: PartView) {
    const p = i.props;
    if (isLayerClass(i.className)) {
      v.group.visible = p.Visible !== false;
      this.world.update(i);
      return;
    }
    const visible = p.Visible !== false && (p.Transparency as number) < 1;
    v.group.visible = p.Visible !== false && !((p.Transparency as number) >= 1 && this.mode === 'play');
    const mat = this.materials.get({
      material: p.Material as string, color: p.Color as string, transparency: Math.min(1, p.Transparency as number),
      reflectance: p.Reflectance as number, textureId: p.TextureId as string, tiling: p.TextureTiling as number,
    });
    const castShadow = !!p.CastShadow && visible;
    if (i.className === 'MeshPart' && p.MeshId) {
      const useOwn = !!p.UseModelMaterials;
      v.mesh.traverse((o) => {
        const m = o as THREE.Mesh;
        if (!m.isMesh) return;
        const placeholder = m.material instanceof THREE.MeshBasicMaterial && (m.material as THREE.MeshBasicMaterial).wireframe;
        if (!useOwn && !placeholder) m.material = mat;
        m.castShadow = castShadow;
        m.receiveShadow = true;
      });
    } else {
      const geo = i.className === 'MeshPart' ? GEOMETRY.Block : GEOMETRY[shapeOf(i)];
      if (!mat.transparent) {
        // opaque parts: GPU instancing (one draw call per geometry/material combination)
        if ((v.mesh as THREE.Mesh).isMesh) { v.mesh.removeFromParent(); this.pickables.delete(v.mesh); v.mesh = new THREE.Object3D(); v.body.add(v.mesh); }
        v.group.updateMatrixWorld(true);
        this.batcher.set(i, geo, mat, castShadow, v.body.matrixWorld, v.group.visible);
      } else {
        // transparent parts need per-object sorting: keep an individual mesh
        this.batcher.remove(i);
        let mesh = v.mesh as THREE.Mesh;
        if (!mesh.isMesh || mesh.geometry !== geo) {
          v.mesh.removeFromParent();
          this.pickables.delete(v.mesh);
          mesh = new THREE.Mesh(geo);
          mesh.userData.inst = i;
          v.body.add(mesh);
          this.pickables.add(mesh);
          v.mesh = mesh;
        }
        mesh.material = mat;
        mesh.castShadow = castShadow;
        mesh.receiveShadow = true;
      }
    }
    if (v.decal) v.decal.visible = (p.Transparency as number) < 1;
  }

  private updatePartTransform(i: Instance, v: PartView) {
    const p = i.props;
    v.group.position.copy(toThree(p.Position as Vec3));
    orientationToQuat(p.Orientation as Vec3, v.group.quaternion);
    const s = effectiveSize(i);
    v.body.scale.set(Math.max(s.x, 0.001), Math.max(s.y, 0.001), Math.max(s.z, 0.001));
    if (v.decal) {
      const d = Math.min(s.x, s.z) * 0.7;
      v.decal.scale.set(d, d, 1);
      v.decal.position.set(0, s.y / 2 + 0.02, 0);
    }
    v.group.updateMatrixWorld(true);
    if (isLayerClass(i.className)) this.world.update(i);
    if (this.batcher.has(i)) this.batcher.setMatrix(i, v.body.matrixWorld, v.group.visible);
    for (const c of i.children) if (c.className === 'Decal') this.updateDecal(c);
  }

  /** The face of a part (Front/Back/Top/…) that points most toward the camera. */
  faceTowardCamera(inst: Instance): string {
    const v = this.views.get(inst);
    if (v?.kind !== 'part') return 'Front';
    const q = v.group.getWorldQuaternion(new THREE.Quaternion()).invert();
    const d = this.camera.position.clone().sub(v.group.getWorldPosition(new THREE.Vector3())).applyQuaternion(q);
    const ax = Math.abs(d.x), ay = Math.abs(d.y), az = Math.abs(d.z);
    if (ay >= ax && ay >= az) return d.y > 0 ? 'Top' : 'Bottom';
    if (ax >= az) return d.x > 0 ? 'Right' : 'Left';
    return d.z > 0 ? 'Back' : 'Front';
  }

  /** Places a Decal's image on the chosen face of its parent part. */
  private updateDecal(i: Instance) {
    const v = this.views.get(i);
    if (v?.kind !== 'decal' || !i.parent?.isA('BasePart')) return;
    const s = effectiveSize(i.parent);
    const p = i.props;
    const faces: Record<string, { n: [number, number, number]; w: number; h: number; rot: [number, number, number] }> = {
      Front: { n: [0, 0, -1], w: s.x, h: s.y, rot: [0, Math.PI, 0] },
      Back: { n: [0, 0, 1], w: s.x, h: s.y, rot: [0, 0, 0] },
      Top: { n: [0, 1, 0], w: s.x, h: s.z, rot: [-Math.PI / 2, 0, 0] },
      Bottom: { n: [0, -1, 0], w: s.x, h: s.z, rot: [Math.PI / 2, 0, 0] },
      Left: { n: [-1, 0, 0], w: s.z, h: s.y, rot: [0, -Math.PI / 2, 0] },
      Right: { n: [1, 0, 0], w: s.z, h: s.y, rot: [0, Math.PI / 2, 0] },
    };
    const f = faces[p.Face as string] ?? faces.Front;
    const half = Math.abs(f.n[0]) * s.x / 2 + Math.abs(f.n[1]) * s.y / 2 + Math.abs(f.n[2]) * s.z / 2;
    v.mesh.position.set(f.n[0] * (half + 0.01), f.n[1] * (half + 0.01), f.n[2] * (half + 0.01));
    v.mesh.rotation.set(...f.rot);
    v.mesh.scale.set(f.w, f.h, 1);
    const tex = this.materials.textureForAsset(p.Texture as string);
    // decals show the whole image once (no tiling)
    if (tex && v.material.map !== tex) { v.material.map = tex; v.material.needsUpdate = true; }
    v.material.color.set(p.Color3 as string);
    v.material.opacity = 1 - (p.Transparency as number);
    v.mesh.visible = !!tex && (p.Transparency as number) < 1;
  }

  private applyCameraTransform(i: Instance) {
    const v = this.views.get(i);
    if (v?.kind !== 'camera') return;
    v.group.position.copy(toThree(i.props.Position as Vec3));
    orientationToQuat(i.props.Orientation as Vec3, v.group.quaternion);
  }

  private updateLight(i: Instance) {
    const v = this.views.get(i);
    if (v?.kind !== 'light') return;
    const p = i.props;
    const l = v.light;
    l.visible = !!p.Enabled;
    l.color.set(p.Color as string);
    l.intensity = (p.Brightness as number) * 12;
    l.distance = p.Range as number;
    l.decay = 1.2;
    l.castShadow = !!p.Shadows;
    if (l.castShadow) { l.shadow.mapSize.set(512, 512); l.shadow.bias = -0.002; }
    if (l instanceof THREE.SpotLight && v.target) {
      l.angle = Math.min(Math.PI / 2 - 0.01, ((p.Angle as number) * DEG) / 2);
      l.penumbra = 0.3;
      const dirs: Record<string, [number, number, number]> = { Front: [0, 0, -1], Back: [0, 0, 1], Top: [0, 1, 0], Bottom: [0, -1, 0], Left: [-1, 0, 0], Right: [1, 0, 0] };
      v.target.position.set(...(dirs[p.Face as string] ?? dirs.Front));
    }
  }

  private onChanged(i: Instance, prop: string) {
    const v = this.views.get(i);
    if (prop === 'Name' || prop === 'Source') return;
    if (v?.kind === 'part') {
      if (prop === 'Position' || prop === 'Orientation') { this.updatePartTransform(i, v); this.syncPivotIfIdle(i); }
      else if (prop === 'Size' || prop === 'Shape') { if (prop === 'Shape') this.buildPartMesh(i, v); this.updatePartTransform(i, v); this.refreshSelectionVisuals(); }
      else if (prop === 'MeshId' || prop === 'UseModelMaterials') this.buildPartMesh(i, v);
      else this.updatePartLook(i, v);
    } else if (v?.kind === 'light') this.updateLight(i);
    else if (v?.kind === 'emitter') v.ps.refreshLook();
    else if (v?.kind === 'decal') this.updateDecal(i);
    else if (v?.kind === 'camera') { this.applyCameraTransform(i); this.syncPivotIfIdle(i); }
    else if (v?.kind === 'terrain') v.tv.onPropChanged(prop);
  }

  // ---- selection visuals ----------------------------------------------------------

  private selectionList(): readonly Instance[] { return this.doc?.selection.list ?? []; }

  refreshSelectionVisuals() {
    for (const [inst, v] of this.views) if (v.kind === 'camera') v.group.visible = this.mode === 'edit' && this.selectionList().includes(inst);
    for (const o of this.outlines.values()) o.removeFromParent();
    this.outlines.clear();
    for (const s of this.selectionList()) {
      const v = this.views.get(s);
      if (v?.kind === 'part') {
        const shape = s.className === 'MeshPart' ? 'Block' : shapeOf(s);
        const line = new THREE.LineSegments(OUTLINES[shape], new THREE.LineBasicMaterial({ color: SELECT_COLOR }));
        line.scale.setScalar(1.002);
        line.raycast = () => {};
        v.body.add(line);
        this.outlines.set(s, line);
      } else if (s.className === 'Model' || s.className === 'Folder') {
        const helper = new THREE.Box3Helper(new THREE.Box3(), new THREE.Color(SELECT_COLOR));
        helper.userData.model = s;
        this.scene.add(helper);
        this.outlines.set(s, helper);
      }
    }
    this.updateGizmo();
  }

  private setHover(i: Instance | null) {
    this.hovered = i;
    this.hoverOutline.visible = false;
  }

  private updateHoverOutline() {
    const i = this.hovered;
    if (!i || this.mode !== 'edit' || this.selectionList().includes(i)) { this.hoverOutline.visible = false; return; }
    const parts = i.isA('BasePart') ? [i] : collectTransformables([i]);
    if (!parts.length) { this.hoverOutline.visible = false; return; }
    if (i.isA('BasePart')) {
      const s = effectiveSize(i);
      instanceMatrix(i, this.hoverOutline.matrix);
      this.hoverOutline.matrix.scale(new THREE.Vector3(s.x * 1.004, s.y * 1.004, s.z * 1.004));
    } else {
      const b = boundsOf(parts);
      const c = b.getCenter(new THREE.Vector3()), sz = b.getSize(new THREE.Vector3());
      this.hoverOutline.matrix.compose(c, new THREE.Quaternion(), sz);
    }
    this.hoverOutline.matrixAutoUpdate = false;
    this.hoverOutline.matrixWorldNeedsUpdate = true;
    this.hoverOutline.visible = true;
  }

  // ---- gizmo -------------------------------------------------------------------

  setTool(t: Tool) {
    this.tool = t;
    this.brushRing.visible = false;
    this.worldTools.toolChanged();
    this.updateGizmo();
  }

  setSpace(s: 'world' | 'local') { this.space = s; this.tc.setSpace(s); this.updateGizmo(); }

  private snap() { return this.doc?.settings ?? { snapEnabled: false, moveSnap: 1, rotateSnap: 15, gridSize: 4 }; }

  updateGizmo() {
    const editing = this.mode === 'edit' && !!this.doc;
    const parts = editing ? collectTransformables(this.selectionList()).filter((p) => !p.props.Locked || !p.isA('BasePart')) : [];
    const gizmoTool = this.tool === 'move' || this.tool === 'rotate';
    if (!editing || !gizmoTool || parts.length === 0) {
      if (!this.gizmoStart) this.tc.detach();
    } else if (!this.gizmoStart) {
      this.placePivot(parts);
      this.tc.setMode(this.tool === 'move' ? 'translate' : 'rotate');
      this.tc.setSpace(this.space);
      const s = this.snap();
      this.tc.setTranslationSnap(null);
      this.tc.setRotationSnap(s.snapEnabled && s.rotateSnap > 0 ? s.rotateSnap * DEG : null);
      if (this.tc.object !== this.pivot) this.tc.attach(this.pivot);
    }
    this.scaleHandles.visible = editing && this.tool === 'scale' && !!this.scaleTarget();
  }

  private placePivot(parts: Instance[]) {
    if (parts.length === 1) {
      this.pivot.position.copy(toThree(parts[0].props.Position as Vec3));
      orientationToQuat(parts[0].props.Orientation as Vec3, this.pivot.quaternion);
    } else {
      boundsOf(parts).getCenter(this.pivot.position);
      this.pivot.quaternion.identity();
    }
    this.pivot.scale.set(1, 1, 1);
    this.pivot.updateMatrixWorld(true);
  }

  private syncPivotIfIdle(i: Instance) {
    if (this.gizmoStart || this.moveDrag || this.scaleDrag) return;
    if (this.selectionList().length && collectTransformables(this.selectionList()).includes(i)) this.updateGizmo();
  }

  private gizmoBegin() {
    const parts = collectTransformables(this.selectionList());
    this.pivot.updateMatrixWorld(true);
    this.gizmoStart = { pivot: this.pivot.matrixWorld.clone(), parts, mats: parts.map((p) => instanceMatrix(p)) };
  }

  private gizmoChange() {
    const g = this.gizmoStart;
    if (!g) return;
    const s = this.snap();
    if (this.tc.mode === 'translate' && s.snapEnabled && s.moveSnap > 0) {
      // relative snapping: snap the pivot's displacement, not its absolute position
      const start = new THREE.Vector3().setFromMatrixPosition(g.pivot);
      const d = this.pivot.position.clone().sub(start);
      if (this.space === 'local') {
        const q = new THREE.Quaternion().setFromRotationMatrix(g.pivot);
        d.applyQuaternion(q.clone().invert());
        d.set(Math.round(d.x / s.moveSnap) * s.moveSnap, Math.round(d.y / s.moveSnap) * s.moveSnap, Math.round(d.z / s.moveSnap) * s.moveSnap);
        d.applyQuaternion(q);
      } else d.set(Math.round(d.x / s.moveSnap) * s.moveSnap, Math.round(d.y / s.moveSnap) * s.moveSnap, Math.round(d.z / s.moveSnap) * s.moveSnap);
      this.pivot.position.copy(start).add(d);
    }
    this.pivot.updateMatrixWorld(true);
    const delta = this.pivot.matrixWorld.clone().multiply(g.pivot.clone().invert());
    const pos = new THREE.Vector3(), q = new THREE.Quaternion(), sc = new THREE.Vector3();
    g.parts.forEach((p, k) => {
      const m = delta.clone().multiply(g.mats[k]);
      m.decompose(pos, q, sc);
      p.setSilent('Position', v3(round3(pos.x), round3(pos.y), round3(pos.z)));
      p.setSilent('Orientation', quatToOrientation(q));
    });
  }

  private gizmoEnd() {
    const g = this.gizmoStart;
    this.gizmoStart = null;
    if (!g || !this.doc) return;
    this.commitTransforms(g.parts, g.mats.map((m) => {
      const pos = new THREE.Vector3(), q = new THREE.Quaternion(), s = new THREE.Vector3();
      m.decompose(pos, q, s);
      return { Position: toVec3(pos), Orientation: quatToOrientation(q) };
    }), this.tc.mode === 'translate' ? 'Move' : 'Rotate');
    this.updateGizmo();
  }

  /** Re-applies the final transform inside a history transaction (start → final). */
  private commitTransforms(parts: Instance[], starts: Record<string, Vec3>[], label: string) {
    const finals = parts.map((p) => ({ Position: p.props.Position as Vec3, Orientation: p.props.Orientation as Vec3, Size: p.props.Size as Vec3 }));
    parts.forEach((p, k) => { for (const [key, val] of Object.entries(starts[k])) p.setSilent(key, val); });
    this.doc!.transact(label, () => {
      parts.forEach((p, k) => { for (const key of Object.keys(starts[k])) p.set(key, finals[k][key as 'Position']); });
    });
  }

  // ---- scale handles -------------------------------------------------------------

  private buildScaleHandles() {
    const geo = new THREE.SphereGeometry(0.35, 16, 12);
    const axes: [number, number, number, number][] = [[1, 0, 0, 0xff4d4d], [-1, 0, 0, 0xff4d4d], [0, 1, 0, 0x4dff6a], [0, -1, 0, 0x4dff6a], [0, 0, 1, 0x4d8dff], [0, 0, -1, 0x4d8dff]];
    for (const [x, y, z, c] of axes) {
      const m = new THREE.Mesh(geo, new THREE.MeshBasicMaterial({ color: c, depthTest: false, transparent: true, opacity: 0.95 }));
      m.renderOrder = 1000;
      m.userData.axis = new THREE.Vector3(x, y, z);
      this.scaleHandles.add(m);
    }
    this.scaleHandles.visible = false;
    this.scene.add(this.scaleHandles);
  }

  private scaleTarget(): Instance | undefined {
    const p = this.doc?.selection.primary;
    return p && p.isA('BasePart') && !p.props.Locked ? p : undefined;
  }

  private updateScaleHandles() {
    const t = this.scaleTarget();
    if (!this.scaleHandles.visible || !t) return;
    const s = effectiveSize(t);
    const m = instanceMatrix(t);
    const q = new THREE.Quaternion().setFromRotationMatrix(m);
    const center = toThree(t.props.Position as Vec3);
    for (const h of this.scaleHandles.children) {
      const a = h.userData.axis as THREE.Vector3;
      const half = Math.abs(a.x) * s.x / 2 + Math.abs(a.y) * s.y / 2 + Math.abs(a.z) * s.z / 2;
      const dist = this.camera.position.distanceTo(center);
      const scale = Math.max(0.8, dist * 0.04);
      h.position.copy(a).multiplyScalar(half + scale * 1.2).applyQuaternion(q).add(center);
      h.scale.setScalar(scale);
    }
  }

  private closestAxisParam(origin: THREE.Vector3, axis: THREE.Vector3): number {
    const ray = this.raycaster.ray;
    const w0 = origin.clone().sub(ray.origin);
    const a = axis.dot(axis), b = axis.dot(ray.direction), c = ray.direction.dot(ray.direction);
    const d = axis.dot(w0), e = ray.direction.dot(w0);
    const den = a * c - b * b;
    if (Math.abs(den) < 1e-6) return 0;
    return (b * e - c * d) / den;
  }

  // ---- picking --------------------------------------------------------------------

  private setRay(clientX: number, clientY: number) {
    const r = this.canvas.getBoundingClientRect();
    this.pointer.set(((clientX - r.left) / r.width) * 2 - 1, -((clientY - r.top) / r.height) * 2 + 1);
    this.raycaster.setFromCamera(this.pointer, this.camera);
  }

  /** Raycast from screen coordinates. Returns the first hit (skipping locked parts in edit mode). */
  pick(clientX: number, clientY: number, opts: { includeTerrain?: boolean; exclude?: Set<Instance>; includeLocked?: boolean } = {}) {
    this.setRay(clientX, clientY);
    const ray = this.raycaster.ray;
    // terrain is ray-marched in the Rust core instead of testing every triangle
    let terrainHit: { inst: Instance; point: THREE.Vector3; normal: THREE.Vector3; terrain: true; distance: number } | null = null;
    const tInst = this.dm?.terrainInstance;
    if (opts.includeTerrain && tInst?.terrain && this.views.has(tInst)) {
      const d = tInst.terrain.raycast(ray.origin.x, ray.origin.y, ray.origin.z, ray.direction.x, ray.direction.y, ray.direction.z, 5000);
      if (d !== null) {
        const point = ray.origin.clone().addScaledVector(ray.direction, d);
        terrainHit = { inst: tInst, point, normal: new THREE.Vector3(...tInst.terrain.normalAt(point.x, point.z)), terrain: true, distance: d };
      }
    }
    const hits = this.raycaster.intersectObjects([...this.pickables, ...this.batcher.meshes], false);
    let wh = this.world.raycast(this.raycaster);
    if (wh && (opts.exclude?.has(wh.inst) || (this.mode === 'edit' && wh.inst.props.Locked && !opts.includeLocked))) wh = null;
    for (const h of hits) {
      if (terrainHit && h.distance > terrainHit.distance) break;
      if (wh && h.distance > wh.distance) break;
      let o: THREE.Object3D | null = h.object, vis = true;
      while (o) { if (!o.visible) { vis = false; break; } o = o.parent; }
      if (!vis) continue;
      const inst = (h.object.userData.inst as Instance | undefined) ?? this.batcher.instanceFor(h.object, h.instanceId);
      if (!inst) continue;
      if (opts.exclude?.has(inst)) continue;
      if (this.mode === 'edit' && inst.props.Locked && !opts.includeLocked) continue;
      const n = h.face ? h.face.normal.clone().transformDirection(h.object.matrixWorld) : new THREE.Vector3(0, 1, 0);
      return { inst, point: h.point, normal: n, terrain: false };
    }
    if (wh && (!terrainHit || wh.distance <= terrainHit.distance)) return { inst: wh.inst, point: wh.point, normal: wh.normal, terrain: false };
    return terrainHit;
  }

  /** Viewport selection target: the outermost Model below Workspace (Alt selects the part). */
  selectTarget(inst: Instance, alt: boolean): Instance {
    if (alt || !this.dm) return inst;
    const ws = this.dm.workspace;
    let target = inst;
    let p = inst.parent;
    while (p && p !== ws) { if (p.className === 'Model') target = p; p = p.parent; }
    return target;
  }

  /** World position at the center of the screen for inserting new objects. */
  insertPosition(size: Vec3): Vec3 {
    const r = this.canvas.getBoundingClientRect();
    const hit = this.pick(r.left + r.width / 2, r.top + r.height / 2, { includeTerrain: true, includeLocked: true });
    const snap = this.snap();
    const g = snap.snapEnabled ? Math.max(snap.moveSnap, 0.001) : 0.001;
    const rs = (x: number) => Math.round(x / g) * g;
    if (hit && hit.point.distanceTo(this.camera.position) < 400) {
      const n = hit.normal;
      const off = Math.abs(n.x) * size.x / 2 + Math.abs(n.y) * size.y / 2 + Math.abs(n.z) * size.z / 2;
      const p = hit.point.clone().addScaledVector(n, off);
      return v3(rs(p.x), round3(p.y), rs(p.z));
    }
    const dir = new THREE.Vector3();
    this.camera.getWorldDirection(dir);
    const p = this.camera.position.clone().addScaledVector(dir, 24);
    return v3(rs(p.x), Math.max(size.y / 2, round3(p.y)), rs(p.z));
  }

  focusOn(insts: readonly Instance[]) {
    const parts = collectTransformables(insts);
    if (!parts.length) return;
    const b = boundsOf(parts);
    const c = b.getCenter(new THREE.Vector3());
    const r = Math.max(2, b.getSize(new THREE.Vector3()).length() / 2);
    const dir = this.forward();
    this.camPos.copy(c).addScaledVector(dir, -(r * 2.2 + 4));
  }

  forward() {
    return new THREE.Vector3(-Math.sin(this.yaw) * Math.cos(this.pitch), Math.sin(this.pitch), -Math.cos(this.yaw) * Math.cos(this.pitch));
  }

  // ---- input -----------------------------------------------------------------------

  private bindInput() {
    const c = this.canvas;
    c.addEventListener('contextmenu', (e) => e.preventDefault());
    c.addEventListener('pointerdown', (e) => this.onPointerDown(e));
    window.addEventListener('pointermove', (e) => this.onPointerMove(e));
    window.addEventListener('pointerup', (e) => this.onPointerUp(e));
    c.addEventListener('pointerenter', () => { this.pointerInside = true; });
    c.addEventListener('pointerleave', () => { this.pointerInside = false; if (!this.drag) { this.setHover(null); this.brushRing.visible = false; } });
    c.addEventListener('wheel', (e) => this.onWheel(e), { passive: false });
    // Toolbox drag & drop: show where the object will land, insert on drop
    const dropPoint = (e: DragEvent) => {
      const hit = this.pick(e.clientX, e.clientY, { includeTerrain: true, includeLocked: true });
      if (hit) return hit.point.clone();
      this.setRay(e.clientX, e.clientY);
      const p = new THREE.Vector3();
      return this.raycaster.ray.intersectPlane(new THREE.Plane(new THREE.Vector3(0, 1, 0), 0), p) ? p : null;
    };
    window.addEventListener('keydown', (e) => {
      if (!this.moveDrag || e.ctrlKey || e.metaKey) return;
      if (e.key === 'r' || e.key === 'R') { e.preventDefault(); this.rotateDragged('y'); }
      if (e.key === 't' || e.key === 'T') { e.preventDefault(); this.rotateDragged('x'); }
    });
    c.addEventListener('dragover', (e) => {
      const types = e.dataTransfer?.types ?? [];
      if (this.mode !== 'edit' || !(types.includes('text/forge-toolbox') || types.includes('Files'))) return;
      e.preventDefault();
      e.dataTransfer!.dropEffect = 'copy';
      const p = dropPoint(e);
      this.brushRing.visible = !!p;
      if (p) { this.brushRing.position.copy(p).add(new THREE.Vector3(0, 0.15, 0)); this.brushRing.scale.setScalar(2); }
    });
    c.addEventListener('dragleave', () => { if (this.tool !== 'terrain') this.brushRing.visible = false; });
    c.addEventListener('drop', (e) => {
      const id = e.dataTransfer?.getData('text/forge-toolbox');
      if (this.tool !== 'terrain') this.brushRing.visible = false;
      if (this.mode !== 'edit') return;
      const files = [...(e.dataTransfer?.files ?? [])];
      if (!id && !files.length) return;
      e.preventDefault();
      const p = dropPoint(e);
      const at = p ? v3(p.x, p.y, p.z) : undefined;
      if (id && at) this.input.onToolboxDrop?.(id, at);
      if (files.length) this.input.onFilesDrop?.(files, at);
      this.canvas.focus();
    });
    c.addEventListener('keydown', (e) => {
      if (e.ctrlKey || e.metaKey || e.altKey) return;
      const k = e.key.toLowerCase();
      if (this.mode === 'edit' && ['w', 'a', 's', 'd', 'q', 'e', 'shift', 'arrowup', 'arrowdown', 'arrowleft', 'arrowright'].includes(k)) this.keys.add(k);
    });
    c.addEventListener('keyup', (e) => this.keys.delete(e.key.toLowerCase()));
    c.addEventListener('blur', () => this.keys.clear());
  }

  private onPointerDown(e: PointerEvent) {
    this.canvas.focus();
    if (this.mode === 'play' && this.playPointer) { this.playPointer(e, 'down'); return; }
    if (this.mode !== 'edit') return;
    if (e.button === 2) { this.drag = { kind: 'look', x: e.clientX, y: e.clientY, moved: false, button: 2 }; return; }
    if (e.button === 1) { e.preventDefault(); this.drag = { kind: 'pan', x: e.clientX, y: e.clientY, moved: false, button: 1 }; return; }
    if (e.button !== 0) return;
    // gizmo has priority when one of its handles is hovered
    if (this.tc.object && this.tc.axis !== null) return;
    if (this.worldTools.pointerDown(e)) return;
    this.setRay(e.clientX, e.clientY);
    if (this.tool === 'scale' && this.scaleHandles.visible) {
      const h = this.raycaster.intersectObjects(this.scaleHandles.children, false)[0];
      const t = this.scaleTarget();
      if (h && t) {
        const local = h.object.userData.axis as THREE.Vector3;
        const q = orientationToQuat(t.props.Orientation as Vec3);
        const axis = local.clone().applyQuaternion(q).normalize();
        const origin = toThree(t.props.Position as Vec3);
        this.scaleDrag = {
          inst: t, axis, local: Math.abs(local.x) ? 0 : Math.abs(local.y) ? 1 : 2, sign: local.x + local.y + local.z,
          startSize: t.props.Size as Vec3, startPos: t.props.Position as Vec3, t0: this.closestAxisParam(origin, axis), origin,
        };
        return;
      }
    }
    if (this.tool === 'terrain') { this.beginBrush(e); return; }
    this.drag = { kind: 'left', x: e.clientX, y: e.clientY, moved: false, button: 0 };
  }

  private onPointerMove(e: PointerEvent) {
    if (this.mode === 'play') { if (this.playPointer) this.playPointer(e, 'move'); return; }
    if ((e.target === this.canvas || this.worldTools.dragging) && this.worldTools.pointerMove(e)) return;
    const d = this.drag;
    if (d) {
      const dx = e.clientX - d.x, dy = e.clientY - d.y;
      if (Math.abs(dx) + Math.abs(dy) > 3) d.moved = true;
      if (d.kind === 'look') {
        this.yaw -= e.movementX * 0.005;
        this.pitch = THREE.MathUtils.clamp(this.pitch - e.movementY * 0.005, -1.55, 1.55);
      } else if (d.kind === 'pan') {
        const speed = 0.05 + this.camPos.y * 0.002;
        const right = new THREE.Vector3(Math.cos(this.yaw), 0, -Math.sin(this.yaw));
        const up = new THREE.Vector3(0, 1, 0).applyAxisAngle(right, 0);
        this.camPos.addScaledVector(right, -e.movementX * speed * 2).addScaledVector(up, e.movementY * speed * 2);
      } else if (d.kind === 'left' && d.moved) {
        if (!this.moveDrag && !this.marqueeStart) this.beginLeftDrag(d.x, d.y, e);
        if (this.moveDrag) this.updateMoveDrag(e);
        if (this.marqueeStart) this.updateMarquee(e);
      }
      return;
    }
    if (this.scaleDrag) { this.updateScaleDrag(e); return; }
    if (e.target !== this.canvas) return;
    if (this.tool === 'terrain') { this.updateBrushCursor(e); return; }
    const hit = this.pick(e.clientX, e.clientY);
    this.hovered = hit ? this.selectTarget(hit.inst, e.altKey) : null;
  }

  private onPointerUp(e: PointerEvent) {
    if (this.mode === 'play') { if (this.playPointer) this.playPointer(e, 'up'); return; }
    if (this.brushStroke && e.button === 0) { this.endBrush(); return; }
    if (this.worldTools.pointerUp(e)) return;
    if (this.scaleDrag && e.button === 0) { this.endScaleDrag(); return; }
    const d = this.drag;
    if (!d || d.button !== e.button) return;
    this.drag = null;
    if (d.kind === 'look' && !d.moved) {
      const hit = this.pick(e.clientX, e.clientY);
      let target = hit ? this.selectTarget(hit.inst, e.altKey) : null;
      if (target && this.doc && !this.doc.selection.has(target)) this.doc.selection.set([target]);
      if (!target && this.doc?.selection.size) target = this.doc.selection.primary ?? null;
      this.input.onContextMenu?.(e.clientX, e.clientY, target);
      return;
    }
    if (d.kind !== 'left') return;
    if (this.moveDrag) { this.endMoveDrag(); return; }
    if (this.marqueeStart) { this.endMarquee(e); return; }
    if (!d.moved) this.clickSelect(e);
  }

  private clickSelect(e: PointerEvent) {
    const sel = this.doc?.selection;
    if (!sel) return;
    const hit = this.pick(e.clientX, e.clientY);
    const target = hit ? this.selectTarget(hit.inst, e.altKey) : null;
    const add = e.ctrlKey || e.shiftKey || e.metaKey;
    if (!target) { if (!add) sel.clear(); return; }
    if (add) sel.toggle(target);
    else sel.set([target]);
  }

  private beginLeftDrag(x: number, y: number, e: PointerEvent) {
    const hit = this.pick(x, y);
    const sel = this.doc?.selection;
    if (!sel) return;
    if (hit) {
      const target = this.selectTarget(hit.inst, e.altKey);
      if (!sel.has(target)) sel.set([target]);
      const parts = collectTransformables(sel.list).filter((p) => p.isA('BasePart') && !p.props.Locked);
      if (!parts.length) return;
      const b = boundsOf(parts);
      this.moveDrag = {
        parts, starts: parts.map((p) => p.props.Position as Vec3), orients: parts.map((p) => p.props.Orientation as Vec3),
        plane: new THREE.Plane(new THREE.Vector3(0, 1, 0), -hit.point.y), startPoint: hit.point.clone(), bottom: b.min.y, changed: false,
        center: b.getCenter(new THREE.Vector3()), turn: new THREE.Quaternion(), delta: new THREE.Vector3(), lastEvent: e,
      };
      this.canvas.style.cursor = 'grabbing';
    } else {
      this.marqueeStart = { x, y };
    }
  }

  private updateMoveDrag(e: PointerEvent) {
    const md = this.moveDrag!;
    const exclude = new Set(md.parts);
    const hit = this.pick(e.clientX, e.clientY, { includeTerrain: true, exclude, includeLocked: true });
    this.setRay(e.clientX, e.clientY);
    const p = new THREE.Vector3();
    let dy = 0;
    if (hit && hit.normal.y > 0.6) {
      p.copy(hit.point);
      dy = hit.point.y - md.bottom;
    } else if (!this.raycaster.ray.intersectPlane(md.plane, p)) return;
    const s = this.snap();
    const g = s.snapEnabled && s.moveSnap > 0 ? s.moveSnap : 0;
    let dx = p.x - md.startPoint.x, dz = p.z - md.startPoint.z;
    if (g) { dx = Math.round(dx / g) * g; dz = Math.round(dz / g) * g; }
    md.delta.set(dx, dy, dz);
    md.lastEvent = e;
    this.applyMoveDrag();
  }

  /** Positions dragged parts: start transform, rotated by R/T presses about the group center, then offset. */
  private applyMoveDrag() {
    const md = this.moveDrag!;
    const p = new THREE.Vector3();
    md.parts.forEach((part, k) => {
      p.copy(toThree(md.starts[k])).sub(md.center).applyQuaternion(md.turn).add(md.center).add(md.delta);
      part.setSilent('Position', v3(round3(p.x), round3(p.y), round3(p.z)));
      if (md.turn.w < 0.99999) part.setSilent('Orientation', quatToOrientation(md.turn.clone().multiply(orientationToQuat(md.orients[k]))));
    });
    md.changed = true;
  }

  /** R = turn 90° around Y, T = tilt 90° around X (while dragging, like Roblox Studio). */
  private rotateDragged(axis: 'y' | 'x') {
    const md = this.moveDrag;
    if (!md) return;
    const q = new THREE.Quaternion().setFromAxisAngle(axis === 'y' ? new THREE.Vector3(0, 1, 0) : new THREE.Vector3(1, 0, 0), Math.PI / 2);
    md.turn.premultiply(q);
    this.applyMoveDrag();
  }

  private endMoveDrag() {
    const md = this.moveDrag!;
    this.moveDrag = null;
    this.canvas.style.cursor = '';
    if (md.changed) this.commitTransforms(md.parts, md.starts.map((s, k) => ({ Position: s, Orientation: md.orients[k] })), 'Drag');
    this.updateGizmo();
  }

  private updateMarquee(e: PointerEvent) {
    const s = this.marqueeStart!;
    const r = this.container.getBoundingClientRect();
    const x0 = Math.min(s.x, e.clientX) - r.left, y0 = Math.min(s.y, e.clientY) - r.top;
    Object.assign(this.marquee.style, { display: 'block', left: `${x0}px`, top: `${y0}px`, width: `${Math.abs(e.clientX - s.x)}px`, height: `${Math.abs(e.clientY - s.y)}px` });
  }

  private endMarquee(e: PointerEvent) {
    const s = this.marqueeStart!;
    this.marqueeStart = null;
    this.marquee.style.display = 'none';
    const r = this.canvas.getBoundingClientRect();
    const x0 = Math.min(s.x, e.clientX), x1 = Math.max(s.x, e.clientX), y0 = Math.min(s.y, e.clientY), y1 = Math.max(s.y, e.clientY);
    const found = new Set<Instance>();
    const v = new THREE.Vector3();
    for (const [inst, view] of this.views) {
      if (view.kind !== 'part' || inst.props.Locked || !view.group.visible) continue;
      v.copy(view.group.position).project(this.camera);
      if (v.z > 1) continue;
      const sx = r.left + (v.x + 1) / 2 * r.width, sy = r.top + (1 - v.y) / 2 * r.height;
      if (sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1) found.add(this.selectTarget(inst, e.altKey));
    }
    const sel = this.doc?.selection;
    if (!sel) return;
    if (e.ctrlKey || e.shiftKey) sel.set([...sel.list, ...found]);
    else sel.set([...found]);
  }

  private updateScaleDrag(e: PointerEvent) {
    const sd = this.scaleDrag!;
    this.setRay(e.clientX, e.clientY);
    const t = this.closestAxisParam(sd.origin, sd.axis);
    let delta = (t - sd.t0) * Math.sign(sd.sign);
    const s = this.snap();
    if (s.snapEnabled && s.moveSnap > 0) delta = Math.round(delta / s.moveSnap) * s.moveSnap;
    const comp = ['x', 'y', 'z'][sd.local] as 'x' | 'y' | 'z';
    const min = s.snapEnabled && s.moveSnap > 0 ? Math.min(s.moveSnap, 0.05) : 0.05;
    const newLen = Math.max(min, sd.startSize[comp] + delta);
    const actual = newLen - sd.startSize[comp];
    const size = { ...sd.startSize, [comp]: round3(newLen) } as Vec3;
    const shift = sd.axis.clone().multiplyScalar(actual / 2);
    const isBall = sd.inst.className !== 'WedgePart' && sd.inst.props.Shape === 'Ball';
    sd.inst.setSilent('Size', isBall ? v3(newLen, newLen, newLen) : size);
    sd.inst.setSilent('Position', v3(round3(sd.startPos.x + shift.x), round3(sd.startPos.y + shift.y), round3(sd.startPos.z + shift.z)));
  }

  private endScaleDrag() {
    const sd = this.scaleDrag!;
    this.scaleDrag = null;
    this.commitTransforms([sd.inst], [{ Size: sd.startSize, Position: sd.startPos }], 'Resize');
  }

  private onWheel(e: WheelEvent) {
    e.preventDefault();
    if (this.mode === 'play') { this.playPointer?.(e, 'wheel'); return; }
    const hit = this.pick(e.clientX, e.clientY, { includeTerrain: true, includeLocked: true });
    const dist = hit ? hit.point.distanceTo(this.camPos) : 30;
    const amount = Math.sign(e.deltaY) * -Math.max(1, dist * 0.15);
    this.setRay(e.clientX, e.clientY);
    this.camPos.addScaledVector(this.raycaster.ray.direction, amount);
  }

  // ---- terrain brush ----------------------------------------------------------------

  private terrainInst() { return this.dm?.terrainInstance; }

  private brushPoint(e: { clientX: number; clientY: number }): THREE.Vector3 | null {
    const hit = this.pick(e.clientX, e.clientY, { includeTerrain: true, includeLocked: true });
    if (hit?.terrain) return hit.point;
    if (hit && this.terrainBrush.tool !== 'raise' && this.terrainBrush.tool !== 'flatten') return hit.point;
    // empty terrain (or aiming past it): use the y=0 ground plane
    this.setRay(e.clientX, e.clientY);
    const p = new THREE.Vector3();
    return this.raycaster.ray.intersectPlane(new THREE.Plane(new THREE.Vector3(0, 1, 0), 0), p) ? p : null;
  }

  private updateBrushCursor(e: { clientX: number; clientY: number }) {
    this.lastPointer = { clientX: e.clientX, clientY: e.clientY };
    const p = this.brushPoint(e);
    this.brushHit = p;
    this.brushRing.visible = !!p;
    if (p) { this.brushRing.position.copy(p).add(new THREE.Vector3(0, 0.2, 0)); this.brushRing.scale.setScalar(this.terrainBrush.radius); }
  }

  private beginBrush(e: PointerEvent) {
    const t = this.terrainInst();
    if (!t?.terrain || !this.doc) return;
    this.lastPointer = { clientX: e.clientX, clientY: e.clientY };
    const p = this.brushPoint(e);
    if (!p) return;
    this.brushHit = p;
    if (this.terrainBrush.tool === 'flatten') this.terrainBrush.flattenHeight = p.y;
    this.brushStroke = { before: t.terrain.snapshot(), inst: t, region: null };
  }

  private stepBrush(dt: number) {
    const st = this.brushStroke;
    if (!st || !st.inst.terrain) return;
    // re-pick every step so the brush follows the surface while it rises/sinks
    if (this.lastPointer) this.updateBrushCursor(this.lastPointer);
    if (!this.brushHit) return;
    const region = applyBrush(st.inst.terrain, this.brushHit.x, this.brushHit.z, this.terrainBrush, Math.min(dt, 0.05));
    if (region) {
      st.region = unionRegion(st.region, region);
      this.dm?.terrainChanged.fire(st.inst, region);
    }
  }

  private endBrush() {
    const st = this.brushStroke!;
    this.brushStroke = null;
    if (!st.region) return;
    // store only the touched rectangle in the undo history
    const r = st.region;
    const w = r.x1 - r.x0 + 1, h = r.z1 - r.z0 + 1, size = st.inst.terrain!.size;
    const before = { region: { ...r }, heights: new Float32Array(w * h), materials: new Uint8Array(w * h), water: new Uint8Array(w * h) };
    for (let z = 0; z < h; z++) {
      const src = (r.z0 + z) * size + r.x0;
      before.heights.set(st.before.heights.subarray(src, src + w), z * w);
      before.materials.set(st.before.materials.subarray(src, src + w), z * w);
      before.water.set(st.before.water.subarray(src, src + w), z * w);
    }
    const after = st.inst.terrain!.snapshot(r);
    this.doc?.transact('Terrain ' + this.terrainBrush.tool, () => this.doc!.history.record({ k: 'terrain', inst: st.inst, before, after }));
  }

  // ---- frame loop -------------------------------------------------------------------

  updateGrid() {
    if (this.grid) { this.scene.remove(this.grid); this.grid.dispose(); this.grid = null; }
    const g = this.doc?.settings.gridSize ?? 4;
    if (!this.showGrid || this.mode !== 'edit' || g <= 0) return;
    const size = 512;
    this.grid = new THREE.GridHelper(size, Math.max(1, Math.round(size / g)), 0x99a3b3, 0x99a3b3);
    const m = this.grid.material as THREE.LineBasicMaterial;
    m.transparent = true; m.opacity = 0.22; m.depthWrite = false;
    this.grid.position.y = 0.02;
    this.scene.add(this.grid);
  }

  private updateEditorCamera(dt: number) {
    const k = this.keys;
    const speed = (k.has('shift') ? 12 : 48) * dt;
    const f = this.forward();
    const right = new THREE.Vector3(Math.cos(this.yaw), 0, -Math.sin(this.yaw));
    if (k.has('w') || k.has('arrowup')) this.camPos.addScaledVector(f, speed);
    if (k.has('s') || k.has('arrowdown')) this.camPos.addScaledVector(f, -speed);
    if (k.has('a') || k.has('arrowleft')) this.camPos.addScaledVector(right, -speed);
    if (k.has('d') || k.has('arrowright')) this.camPos.addScaledVector(right, speed);
    if (k.has('e')) this.camPos.y += speed;
    if (k.has('q')) this.camPos.y -= speed;
    this.camera.position.copy(this.camPos);
    this.camera.lookAt(this.camPos.clone().add(f));
  }

  /**
   * Browsers pause requestAnimationFrame for hidden pages. The game must keep
   * running anyway (a hidden multiplayer host would freeze everyone), so while
   * hidden the simulation (beforeRender) is stepped from a timer without drawing.
   */
  private hiddenTimer = 0;
  private onVisibility = () => {
    clearInterval(this.hiddenTimer);
    this.hiddenTimer = 0;
    if (!document.hidden) { this.lastTime = performance.now(); return; }
    let last = performance.now();
    this.hiddenTimer = window.setInterval(() => {
      const now = performance.now();
      let left = Math.min(2, (now - last) / 1000); // timers in hidden tabs may only fire once per second
      last = now;
      while (left > 0.001) { const dt = Math.min(0.05, left); left -= dt; this.beforeRender.fire(dt); }
    }, 50);
  };

  private loop = () => {
    this.raf = requestAnimationFrame(this.loop);
    const now = performance.now();
    const dt = Math.min(0.1, (now - this.lastTime) / 1000);
    this.lastTime = now;
    if (!this.container.isConnected || this.container.clientWidth === 0) return;
    const t0 = performance.now();

    if (!this.externalCamera) this.updateEditorCamera(dt);
    this.beforeRender.fire(dt);
    if (this.brushStroke) this.stepBrush(dt);
    if (this.mode === 'edit' && this.tool === 'terrain' && this.brushHit && this.brushStroke) {
      this.brushRing.position.set(this.brushHit.x, (this.terrainInst()?.terrain?.heightAt(this.brushHit.x, this.brushHit.z) ?? this.brushHit.y) + 0.2, this.brushHit.z);
    }

    // particles
    for (const [inst, v] of this.views) {
      if (v.kind !== 'emitter') continue;
      const pv = inst.parent ? this.views.get(inst.parent) : undefined;
      if (pv?.kind === 'part') v.ps.update(dt, pv.group.matrixWorld, effectiveSize(inst.parent!));
      else v.ps.update(dt, null, null);
    }
    // model outlines follow their parts
    for (const [inst, o] of this.outlines) {
      if (o instanceof THREE.Box3Helper) {
        const parts = collectTransformables([inst]);
        o.visible = parts.length > 0;
        if (parts.length) o.box.copy(boundsOf(parts));
      }
    }
    this.updateHoverOutline();
    this.updateScaleHandles();
    this.worldTools.update();

    this.env.sky.position.copy(this.camera.position);
    const focus = this.camera.position.clone().addScaledVector(this.camera.getWorldDirection(new THREE.Vector3()), 40);
    this.env.update(focus);
    this.batcher.flush();
    this.world.flush();
    this.renderer.render(this.scene, this.camera);

    const info = this.renderer.info.render;
    this.stats.drawCalls = info.calls;
    this.stats.triangles = info.triangles;
    this.stats.frameMs = performance.now() - t0;
    this.fpsAcc.t += dt; this.fpsAcc.n++;
    if (this.fpsAcc.t >= 0.5) { this.stats.fps = Math.round(this.fpsAcc.n / this.fpsAcc.t); this.fpsAcc = { t: 0, n: 0 }; }
  };

  resize() {
    const w = this.container.clientWidth, h = this.container.clientHeight;
    if (!w || !h) return;
    this.renderer.setSize(w, h, false);
    this.canvas.style.width = w + 'px';
    this.canvas.style.height = h + 'px';
    this.camera.aspect = w / h;
    this.camera.updateProjectionMatrix();
    for (const v of this.views.values()) if (v.kind === 'emitter') v.ps.setViewportHeight(h);
  }

  get partCount() { let n = 0; for (const v of this.views.values()) if (v.kind === 'part') n++; return n; }

  /** World matrix of a part's rendered group (used by the runtime for attachments). */
  partGroup(i: Instance): THREE.Group | null { const v = this.views.get(i); return v?.kind === 'part' ? v.group : null; }

  /** A standalone copy of a part's visual (for glTF export); instanced parts get a real mesh. */
  exportObject(i: Instance): THREE.Object3D | null {
    const v = this.views.get(i);
    if (v?.kind !== 'part') return null;
    const c = v.group.clone(true);
    if (this.batcher.has(i)) {
      const p = i.props;
      const mesh = new THREE.Mesh(i.className === 'MeshPart' ? GEOMETRY.Block : GEOMETRY[shapeOf(i)], this.materials.get({
        material: p.Material as string, color: p.Color as string, transparency: 0, reflectance: p.Reflectance as number, textureId: p.TextureId as string, tiling: p.TextureTiling as number,
      }));
      c.children[0]?.add(mesh); // body group (carries the size scale)
    }
    return c;
  }

  get drawBatches() { return this.batcher.batchCount; }

  /** Renders the current view into a small JPEG data URL (project thumbnails). */
  captureThumbnail(width = 384, height = 216): string | null {
    if (!this.dm || this.container.clientWidth === 0) return null;
    try {
      this.batcher.flush();
      this.world.flush();
      this.renderer.render(this.scene, this.camera);
      const c = document.createElement('canvas');
      c.width = width; c.height = height;
      const g = c.getContext('2d')!;
      const src = this.canvas;
      // cover-fit crop of the viewport
      const scale = Math.max(width / src.width, height / src.height);
      const w = width / scale, h = height / scale;
      g.drawImage(src, (src.width - w) / 2, (src.height - h) / 2, w, h, 0, 0, width, height);
      return c.toDataURL('image/jpeg', 0.82);
    } catch { return null; }
  }

  emitter(i: Instance): ParticleSystem | null { const v = this.views.get(i); return v?.kind === 'emitter' ? v.ps : null; }

  isTransformableInView(i: Instance) { return isTransformable(i) && this.views.has(i); }

  dispose() {
    document.removeEventListener('visibilitychange', this.onVisibility);
    clearInterval(this.hiddenTimer);
    cancelAnimationFrame(this.raf);
    this.resizeObs.disconnect();
    this.unsub.forEach((u) => u());
    this.clearViews();
    this.world.dispose();
    this.tc.dispose();
    this.env.dispose();
    this.gui.dispose();
    this.materials.dispose();
    this.renderer.dispose();
    this.canvas.remove();
  }
}

function round3(x: number) { const r = Math.round(x * 1000) / 1000; return Object.is(r, -0) ? 0 : r; }

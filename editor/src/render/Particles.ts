// CPU-simulated particle emitter rendered as a single THREE.Points object.

import * as THREE from 'three';
import type { Instance } from '../core/Instance';
import type { Vec3 } from '../core/types';

const shapeTextures = new Map<string, THREE.Texture>();
function shapeTexture(shape: string): THREE.Texture {
  let t = shapeTextures.get(shape);
  if (t) return t;
  const c = document.createElement('canvas');
  c.width = c.height = 64;
  const g = c.getContext('2d')!;
  const grad = (r0: number, r1: number) => { const gr = g.createRadialGradient(32, 32, r0, 32, 32, r1); gr.addColorStop(0, 'rgba(255,255,255,1)'); gr.addColorStop(1, 'rgba(255,255,255,0)'); return gr; };
  switch (shape) {
    case 'Square': g.fillStyle = '#fff'; g.fillRect(8, 8, 48, 48); break;
    case 'Spark': g.fillStyle = grad(0, 10); g.fillRect(0, 0, 64, 64); g.fillStyle = 'rgba(255,255,255,.9)'; g.fillRect(30, 2, 4, 60); g.fillRect(2, 30, 60, 4); break;
    case 'Star': {
      g.fillStyle = '#fff'; g.beginPath();
      for (let i = 0; i < 10; i++) { const r = i % 2 ? 12 : 30, a = (i / 10) * Math.PI * 2 - Math.PI / 2; g.lineTo(32 + Math.cos(a) * r, 32 + Math.sin(a) * r); }
      g.fill(); break;
    }
    case 'Smoke':
      for (let i = 0; i < 6; i++) { g.fillStyle = grad(0, 14 + i * 2); g.globalAlpha = 0.35; g.save(); g.translate(Math.sin(i * 2) * 8, Math.cos(i * 3) * 8); g.fillRect(0, 0, 64, 64); g.restore(); }
      break;
    default: g.fillStyle = grad(4, 31); g.fillRect(0, 0, 64, 64);
  }
  t = new THREE.CanvasTexture(c);
  shapeTextures.set(shape, t);
  return t;
}

const VERT = /* glsl */ `
attribute float psize; attribute vec4 pcolor;
varying vec4 vColor;
uniform float scale;
void main() {
  vColor = pcolor;
  vec4 mv = modelViewMatrix * vec4(position, 1.0);
  gl_PointSize = psize * scale / max(-mv.z, 0.1);
  gl_Position = projectionMatrix * mv;
}`;
const FRAG = /* glsl */ `
uniform sampler2D map;
varying vec4 vColor;
void main() {
  vec4 t = texture2D(map, gl_PointCoord);
  gl_FragColor = vec4(vColor.rgb, vColor.a * t.a);
  if (gl_FragColor.a < 0.01) discard;
  #include <colorspace_fragment>
}`;

export class ParticleSystem {
  readonly points: THREE.Points;
  private max: number;
  private pos: Float32Array; private vel: Float32Array; private age: Float32Array; private life: Float32Array;
  private colors: Float32Array; private sizes: Float32Array;
  private geo = new THREE.BufferGeometry();
  private mat: THREE.ShaderMaterial;
  private accum = 0;
  private alive = 0;
  private pending = 0;
  private c0 = new THREE.Color(); private c1 = new THREE.Color();

  constructor(private inst: Instance) {
    this.max = 2500;
    this.pos = new Float32Array(this.max * 3); this.vel = new Float32Array(this.max * 3);
    this.age = new Float32Array(this.max); this.life = new Float32Array(this.max);
    this.colors = new Float32Array(this.max * 4); this.sizes = new Float32Array(this.max);
    this.geo.setAttribute('position', new THREE.BufferAttribute(this.pos, 3).setUsage(THREE.DynamicDrawUsage));
    this.geo.setAttribute('pcolor', new THREE.BufferAttribute(this.colors, 4).setUsage(THREE.DynamicDrawUsage));
    this.geo.setAttribute('psize', new THREE.BufferAttribute(this.sizes, 1).setUsage(THREE.DynamicDrawUsage));
    this.mat = new THREE.ShaderMaterial({
      vertexShader: VERT, fragmentShader: FRAG, transparent: true, depthWrite: false,
      uniforms: { map: { value: shapeTexture('Circle') }, scale: { value: 600 } },
    });
    this.points = new THREE.Points(this.geo, this.mat);
    this.points.frustumCulled = false;
    this.points.renderOrder = 5;
    this.refreshLook();
  }

  refreshLook() {
    const p = this.inst.props;
    this.mat.uniforms.map.value = shapeTexture(p.Shape as string);
    this.mat.blending = (p.LightEmission as number) > 0.5 ? THREE.AdditiveBlending : THREE.NormalBlending;
    this.mat.needsUpdate = true;
  }

  emit(n: number) { this.pending += Math.max(0, Math.min(1000, Math.floor(n))); }
  clear() { this.alive = 0; this.geo.setDrawRange(0, 0); }

  setViewportHeight(h: number) { this.mat.uniforms.scale.value = h * 0.9; }

  /** Advances the simulation. `source` is the parent part's world matrix and size. */
  update(dt: number, source: THREE.Matrix4 | null, size: Vec3 | null) {
    const p = this.inst.props;
    const life = Math.max(0.05, p.Lifetime as number);
    let spawn = this.pending;
    this.pending = 0;
    if (p.Enabled && source) {
      this.accum += (p.Rate as number) * dt;
      const n = Math.floor(this.accum);
      this.accum -= n;
      spawn += n;
    }
    // Integrate & compact
    const acc = p.Acceleration as Vec3;
    const drag = Math.max(0, p.Drag as number);
    const damp = Math.exp(-drag * dt);
    let w = 0;
    for (let i = 0; i < this.alive; i++) {
      const a = this.age[i] + dt;
      if (a >= this.life[i]) continue;
      const i3 = i * 3, w3 = w * 3;
      this.vel[i3] = (this.vel[i3] + acc.x * dt) * damp;
      this.vel[i3 + 1] = (this.vel[i3 + 1] + acc.y * dt) * damp;
      this.vel[i3 + 2] = (this.vel[i3 + 2] + acc.z * dt) * damp;
      this.pos[w3] = this.pos[i3] + this.vel[i3] * dt;
      this.pos[w3 + 1] = this.pos[i3 + 1] + this.vel[i3 + 1] * dt;
      this.pos[w3 + 2] = this.pos[i3 + 2] + this.vel[i3 + 2] * dt;
      this.vel[w3] = this.vel[i3]; this.vel[w3 + 1] = this.vel[i3 + 1]; this.vel[w3 + 2] = this.vel[i3 + 2];
      this.age[w] = a; this.life[w] = this.life[i];
      w++;
    }
    this.alive = w;
    if (spawn > 0 && source && size) this.spawn(Math.min(spawn, this.max - this.alive), source, size, life);

    // Colors / sizes
    this.c0.set(p.Color as string); this.c1.set(p.ColorEnd as string);
    const s0 = p.Size as number, s1 = p.SizeEnd as number, t0 = p.Transparency as number, t1 = p.TransparencyEnd as number;
    for (let i = 0; i < this.alive; i++) {
      const f = this.age[i] / this.life[i];
      const i4 = i * 4;
      this.colors[i4] = this.c0.r + (this.c1.r - this.c0.r) * f;
      this.colors[i4 + 1] = this.c0.g + (this.c1.g - this.c0.g) * f;
      this.colors[i4 + 2] = this.c0.b + (this.c1.b - this.c0.b) * f;
      this.colors[i4 + 3] = 1 - (t0 + (t1 - t0) * f);
      this.sizes[i] = s0 + (s1 - s0) * f;
    }
    this.geo.setDrawRange(0, this.alive);
    (this.geo.attributes.position as THREE.BufferAttribute).needsUpdate = true;
    (this.geo.attributes.pcolor as THREE.BufferAttribute).needsUpdate = true;
    (this.geo.attributes.psize as THREE.BufferAttribute).needsUpdate = true;
  }

  private spawn(n: number, m: THREE.Matrix4, size: Vec3, life: number) {
    const p = this.inst.props;
    const speed = p.Speed as number;
    const spread = ((p.SpreadAngle as number) * Math.PI) / 180;
    const up = new THREE.Vector3(0, 1, 0).transformDirection(m);
    const tmp = new THREE.Vector3(), dir = new THREE.Vector3();
    const q = new THREE.Quaternion().setFromUnitVectors(new THREE.Vector3(0, 1, 0), up);
    for (let k = 0; k < n; k++) {
      const i = this.alive++;
      tmp.set((Math.random() - 0.5) * size.x, (Math.random() - 0.5) * size.y, (Math.random() - 0.5) * size.z).applyMatrix4(m);
      const theta = Math.random() * Math.PI * 2, phi = Math.random() * spread;
      dir.set(Math.sin(phi) * Math.cos(theta), Math.cos(phi), Math.sin(phi) * Math.sin(theta)).applyQuaternion(q);
      this.pos[i * 3] = tmp.x; this.pos[i * 3 + 1] = tmp.y; this.pos[i * 3 + 2] = tmp.z;
      this.vel[i * 3] = dir.x * speed; this.vel[i * 3 + 1] = dir.y * speed; this.vel[i * 3 + 2] = dir.z * speed;
      this.age[i] = 0;
      this.life[i] = life * (0.8 + Math.random() * 0.4);
    }
  }

  dispose() { this.geo.dispose(); this.mat.dispose(); }
}

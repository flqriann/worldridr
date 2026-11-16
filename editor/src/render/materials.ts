// Procedural part materials. Textures are applied with object-space
// triplanar mapping scaled by the part size, so one shared material tiles
// correctly on parts of any size (no per-part UVs needed).

import * as THREE from 'three';
import type { MaterialName } from '../core/classes';
import type { AssetStore } from '../assets/AssetStore';
import { assetDataUrl } from '../assets/AssetStore';

interface MaterialSpec { roughness: number; metalness: number; tiling: number; paint?: (c: CanvasRenderingContext2D, s: number) => void; emissive?: boolean; minTransparency?: number }

let seed = 1;
const rnd = () => { seed = (seed * 16807) % 2147483647; return seed / 2147483647; };
const gray = (v: number, a = 1) => `rgba(${v | 0},${v | 0},${v | 0},${a})`;

function noise(c: CanvasRenderingContext2D, s: number, count: number, size: number, lo: number, hi: number, alpha = 1) {
  for (let i = 0; i < count; i++) { c.fillStyle = gray(lo + rnd() * (hi - lo), alpha); c.fillRect(rnd() * s, rnd() * s, size, size); }
}
const base = (c: CanvasRenderingContext2D, s: number, v: number) => { c.fillStyle = gray(v); c.fillRect(0, 0, s, s); };

const SPECS: Record<MaterialName, MaterialSpec> = {
  Plastic: { roughness: 0.55, metalness: 0, tiling: 4, paint: (c, s) => { base(c, s, 245); noise(c, s, 3000, 2, 225, 255, 0.5); } },
  SmoothPlastic: { roughness: 0.35, metalness: 0, tiling: 4 },
  Wood: { roughness: 0.75, metalness: 0, tiling: 8, paint: (c, s) => {
    base(c, s, 225);
    for (let y = 0; y < s; y += 3) { c.strokeStyle = gray(170 + rnd() * 70, 0.55); c.lineWidth = 1 + rnd() * 2; c.beginPath(); c.moveTo(0, y); for (let x = 0; x <= s; x += 16) c.lineTo(x, y + Math.sin(x / 30 + y / 17) * 3); c.stroke(); }
  } },
  WoodPlanks: { roughness: 0.75, metalness: 0, tiling: 8, paint: (c, s) => {
    for (let i = 0; i < 4; i++) {
      c.fillStyle = gray(200 + rnd() * 50); c.fillRect(0, (i * s) / 4, s, s / 4);
      for (let k = 0; k < 12; k++) { c.strokeStyle = gray(150 + rnd() * 60, 0.35); c.beginPath(); const y = (i * s) / 4 + rnd() * (s / 4); c.moveTo(0, y); c.lineTo(s, y + rnd() * 4 - 2); c.stroke(); }
      c.fillStyle = gray(90); c.fillRect(0, (i * s) / 4, s, 3);
      c.fillRect(((i * 0.37) % 1) * s, (i * s) / 4, 3, s / 4);
    }
  } },
  Brick: { roughness: 0.85, metalness: 0, tiling: 8, paint: (c, s) => {
    base(c, s, 150);
    const rows = 8, cols = 4, h = s / rows, w = s / cols;
    for (let r = 0; r < rows; r++) for (let k = -1; k <= cols; k++) {
      c.fillStyle = gray(205 + rnd() * 50);
      c.fillRect(k * w + (r % 2 ? w / 2 : 0) + 2, r * h + 2, w - 4, h - 4);
    }
    noise(c, s, 2000, 2, 150, 255, 0.3);
  } },
  Concrete: { roughness: 0.9, metalness: 0, tiling: 8, paint: (c, s) => { base(c, s, 225); noise(c, s, 9000, 2, 170, 255, 0.6); noise(c, s, 300, 4, 150, 200, 0.3); } },
  Cobblestone: { roughness: 0.9, metalness: 0, tiling: 8, paint: (c, s) => {
    base(c, s, 120);
    for (let i = 0; i < 70; i++) { c.fillStyle = gray(190 + rnd() * 65); c.beginPath(); c.ellipse(rnd() * s, rnd() * s, 12 + rnd() * 14, 10 + rnd() * 12, rnd() * 3, 0, 7); c.fill(); }
  } },
  Granite: { roughness: 0.6, metalness: 0, tiling: 6, paint: (c, s) => { base(c, s, 220); noise(c, s, 14000, 2, 120, 255, 0.8); } },
  Marble: { roughness: 0.25, metalness: 0, tiling: 12, paint: (c, s) => {
    base(c, s, 245);
    for (let i = 0; i < 14; i++) { c.strokeStyle = gray(150 + rnd() * 60, 0.35); c.lineWidth = 1 + rnd() * 2; c.beginPath(); let x = rnd() * s, y = 0; c.moveTo(x, y); while (y < s) { x += (rnd() - 0.5) * 30; y += 10 + rnd() * 20; c.lineTo(x, y); } c.stroke(); }
  } },
  Slate: { roughness: 0.85, metalness: 0, tiling: 8, paint: (c, s) => {
    base(c, s, 200);
    for (let y = 0; y < s; y += 6 + rnd() * 10) { c.fillStyle = gray(160 + rnd() * 90, 0.6); c.fillRect(0, y, s, 2 + rnd() * 6); }
    noise(c, s, 3000, 2, 150, 240, 0.4);
  } },
  Metal: { roughness: 0.35, metalness: 0.8, tiling: 6, paint: (c, s) => { base(c, s, 225); for (let y = 0; y < s; y++) { c.fillStyle = gray(200 + rnd() * 55, 0.5); c.fillRect(0, y, s, 1); } } },
  DiamondPlate: { roughness: 0.3, metalness: 0.85, tiling: 4, paint: (c, s) => {
    base(c, s, 210);
    for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++) {
      c.save(); c.translate((x + (y % 2) * 0.5) * s / 8, (y + 0.5) * s / 8); c.rotate(y % 2 ? 0.8 : -0.8);
      c.fillStyle = gray(255); c.fillRect(-9, -2.5, 18, 5); c.fillStyle = gray(130, 0.6); c.fillRect(-9, 2.5, 18, 1.5); c.restore();
    }
  } },
  CorrodedMetal: { roughness: 0.8, metalness: 0.5, tiling: 8, paint: (c, s) => {
    base(c, s, 215);
    for (let i = 0; i < 90; i++) { c.fillStyle = `rgba(${140 + rnd() * 60},${90 + rnd() * 40},${50 + rnd() * 30},${0.25 + rnd() * 0.3})`; c.beginPath(); c.arc(rnd() * s, rnd() * s, 4 + rnd() * 18, 0, 7); c.fill(); }
  } },
  Foil: { roughness: 0.2, metalness: 1, tiling: 4, paint: (c, s) => { base(c, s, 230); for (let i = 0; i < 400; i++) { c.fillStyle = gray(170 + rnd() * 85, 0.5); c.beginPath(); c.moveTo(rnd() * s, rnd() * s); c.lineTo(rnd() * s, rnd() * s); c.lineTo(rnd() * s, rnd() * s); c.fill(); } } },
  Grass: { roughness: 0.95, metalness: 0, tiling: 6, paint: (c, s) => { base(c, s, 215); for (let i = 0; i < 6000; i++) { c.fillStyle = gray(160 + rnd() * 95, 0.7); c.fillRect(rnd() * s, rnd() * s, 1, 3 + rnd() * 4); } } },
  Sand: { roughness: 1, metalness: 0, tiling: 6, paint: (c, s) => { base(c, s, 230); noise(c, s, 16000, 1.5, 185, 255, 0.8); } },
  Ice: { roughness: 0.1, metalness: 0, tiling: 10, minTransparency: 0.1, paint: (c, s) => {
    base(c, s, 245);
    for (let i = 0; i < 25; i++) { c.strokeStyle = gray(200, 0.5); c.beginPath(); let x = rnd() * s, y = rnd() * s; c.moveTo(x, y); for (let k = 0; k < 4; k++) { x += (rnd() - 0.5) * 60; y += (rnd() - 0.5) * 60; c.lineTo(x, y); } c.stroke(); }
  } },
  Glass: { roughness: 0.05, metalness: 0.1, tiling: 4, minTransparency: 0.55 },
  Neon: { roughness: 1, metalness: 0, tiling: 4, emissive: true },
  Fabric: { roughness: 1, metalness: 0, tiling: 3, paint: (c, s) => {
    base(c, s, 225);
    for (let i = 0; i < s; i += 4) { c.fillStyle = gray(190, 0.45); c.fillRect(i, 0, 2, s); c.fillStyle = gray(250, 0.35); c.fillRect(0, i, s, 2); }
  } },
};

export function materialSpec(name: string): MaterialSpec { return SPECS[name as MaterialName] ?? SPECS.Plastic; }

const textureCache = new Map<string, THREE.Texture | null>();

function materialTexture(name: string): THREE.Texture | null {
  if (textureCache.has(name)) return textureCache.get(name)!;
  const spec = materialSpec(name);
  if (!spec.paint) { textureCache.set(name, null); return null; }
  const canvas = document.createElement('canvas');
  canvas.width = canvas.height = 256;
  seed = name.split('').reduce((a, ch) => a + ch.charCodeAt(0) * 31, 7);
  spec.paint(canvas.getContext('2d')!, 256);
  const tex = new THREE.CanvasTexture(canvas);
  tex.wrapS = tex.wrapT = THREE.RepeatWrapping;
  tex.colorSpace = THREE.SRGBColorSpace;
  tex.anisotropy = 4;
  textureCache.set(name, tex);
  return tex;
}

/** Thumbnail data URL for the material picker. */
export function materialPreviewUrl(name: string): string | null {
  const t = materialTexture(name);
  return t ? (t.image as HTMLCanvasElement).toDataURL() : null;
}

// ---- Triplanar shader injection -----------------------------------------------

export function applyTriplanar(mat: THREE.MeshStandardMaterial, tiling: number, worldSpace = false) {
  mat.userData.tiling = { value: tiling };
  mat.onBeforeCompile = (shader) => {
    shader.uniforms.triTiling = mat.userData.tiling;
    shader.vertexShader = shader.vertexShader
      .replace('#include <common>', '#include <common>\nvarying vec3 vTriPos;\nvarying vec3 vTriNormal;')
      .replace('#include <begin_vertex>', worldSpace
        ? '#include <begin_vertex>\nvTriPos = (modelMatrix * vec4(position, 1.0)).xyz;\nvTriNormal = normal;'
        : '#include <begin_vertex>\n#ifdef USE_INSTANCING\nmat4 triM = modelMatrix * instanceMatrix;\n#else\nmat4 triM = modelMatrix;\n#endif\nvec3 triScale = vec3(length(triM[0].xyz), length(triM[1].xyz), length(triM[2].xyz));\nvTriPos = position * triScale;\nvTriNormal = normal;');
    shader.fragmentShader = shader.fragmentShader
      .replace('#include <common>', '#include <common>\nvarying vec3 vTriPos;\nvarying vec3 vTriNormal;\nuniform float triTiling;')
      .replace('#include <map_fragment>', `
#ifdef USE_MAP
  vec3 tw = pow(abs(normalize(vTriNormal)), vec3(4.0));
  tw /= (tw.x + tw.y + tw.z);
  vec3 tp = vTriPos / triTiling;
  vec4 texel = texture2D(map, tp.zy) * tw.x + texture2D(map, tp.xz) * tw.y + texture2D(map, tp.xy) * tw.z;
  diffuseColor *= texel;
#endif`);
  };
  mat.customProgramCacheKey = () => (worldSpace ? 'triplanar-world' : 'triplanar-object');
}

// ---- Material cache -------------------------------------------------------------

export interface PartLook { material: string; color: string; transparency: number; reflectance: number; textureId: string; tiling: number }

export class MaterialCache {
  private mats = new Map<string, THREE.MeshStandardMaterial>();
  private assetTextures = new Map<string, THREE.Texture>();
  /** Called when an async texture finished loading (to request a re-render). */
  onTextureLoaded: () => void = () => {};

  constructor(private assets: () => AssetStore | null) {}

  textureForAsset(id: string): THREE.Texture | null {
    if (!id) return null;
    const cached = this.assetTextures.get(id);
    if (cached) return cached;
    const a = this.assets()?.get(id);
    if (!a || a.kind !== 'texture') return null;
    const tex = new THREE.TextureLoader().load(assetDataUrl(a), () => { tex.needsUpdate = true; this.onTextureLoaded(); });
    tex.wrapS = tex.wrapT = THREE.RepeatWrapping;
    tex.colorSpace = THREE.SRGBColorSpace;
    tex.anisotropy = 4;
    this.assetTextures.set(id, tex);
    return tex;
  }

  get(look: PartLook): THREE.MeshStandardMaterial {
    const key = `${look.material}|${look.color}|${look.transparency.toFixed(3)}|${look.reflectance.toFixed(3)}|${look.textureId}|${look.tiling}`;
    let m = this.mats.get(key);
    if (m) return m;
    const spec = materialSpec(look.material);
    const transparency = Math.max(look.transparency, look.transparency < 1 ? spec.minTransparency ?? 0 : 1);
    const color = new THREE.Color(look.color);
    m = new THREE.MeshStandardMaterial({
      color,
      roughness: spec.roughness * (1 - look.reflectance * 0.8),
      metalness: Math.min(1, spec.metalness + look.reflectance * 0.6),
      transparent: transparency > 0,
      opacity: 1 - transparency,
      depthWrite: transparency < 0.5,
    });
    if (spec.emissive) { m.emissive = color.clone(); m.emissiveIntensity = 1.6; m.roughness = 1; }
    const assetTex = this.textureForAsset(look.textureId);
    const tex = assetTex ?? materialTexture(look.material);
    if (tex) { m.map = tex; applyTriplanar(m, assetTex ? look.tiling : spec.tiling); }
    this.mats.set(key, m);
    return m;
  }

  dispose() {
    for (const m of this.mats.values()) m.dispose();
    this.mats.clear();
    for (const t of this.assetTextures.values()) t.dispose();
    this.assetTextures.clear();
  }
}

/** Shared grayscale detail texture for terrain. */
export function terrainDetailTexture(): THREE.Texture {
  return materialTexture('Concrete')!;
}

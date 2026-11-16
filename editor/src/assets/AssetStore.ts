// Per-project asset storage plus resolution of built-in library assets.
// Asset data is kept inside the project file (base64 / text) so a project is
// a single portable file.

import { Signal } from '../core/Signal';
import type { AssetRecord } from '../core/Serializer';
import { newId } from '../core/types';
import { arrayBufferToBase64, base64ToArrayBuffer } from '../core/encoding';
import { getBuiltinAsset } from './builtin';

export type AssetKind = AssetRecord['kind'];

export const MIME_BY_EXT: Record<string, { kind: AssetKind; mime: string; text?: boolean }> = {
  glb: { kind: 'model', mime: 'model/gltf-binary' },
  gltf: { kind: 'model', mime: 'model/gltf+json', text: true },
  forgemodel: { kind: 'model', mime: 'application/x-forge-model', text: true },
  png: { kind: 'texture', mime: 'image/png' },
  jpg: { kind: 'texture', mime: 'image/jpeg' },
  jpeg: { kind: 'texture', mime: 'image/jpeg' },
  webp: { kind: 'texture', mime: 'image/webp' },
  mp3: { kind: 'sound', mime: 'audio/mpeg' },
  wav: { kind: 'sound', mime: 'audio/wav' },
  ogg: { kind: 'sound', mime: 'audio/ogg' },
  lua: { kind: 'script', mime: 'text/x-lua', text: true },
  luau: { kind: 'script', mime: 'text/x-lua', text: true },
  json: { kind: 'animation', mime: 'application/x-forge-animation', text: true },
};

export const EXT_BY_MIME: Record<string, string> = {
  'model/gltf-binary': 'glb', 'model/gltf+json': 'gltf', 'application/x-forge-model': 'forgemodel',
  'image/png': 'png', 'image/jpeg': 'jpg', 'image/webp': 'webp', 'audio/mpeg': 'mp3', 'audio/wav': 'wav',
  'audio/ogg': 'ogg', 'text/x-lua': 'lua', 'application/x-forge-animation': 'json',
};

export class AssetStore {
  private assets = new Map<string, AssetRecord>();
  readonly changed = new Signal();

  constructor(records: AssetRecord[] = []) {
    for (const r of records) if (r && typeof r.id === 'string') this.assets.set(r.id, r);
  }

  list(kind?: AssetKind): AssetRecord[] {
    const all = [...this.assets.values()];
    return kind ? all.filter((a) => a.kind === kind) : all;
  }

  get(id: string): AssetRecord | undefined {
    if (!id) return undefined;
    return this.assets.get(id) ?? getBuiltinAsset(id);
  }

  has(id: string) { return this.assets.has(id); }

  add(rec: Omit<AssetRecord, 'id' | 'created'> & { id?: string }): AssetRecord {
    const full: AssetRecord = { ...rec, id: rec.id ?? 'asset-' + newId(), created: Date.now() };
    this.assets.set(full.id, full);
    this.changed.fire();
    return full;
  }

  /** Imports raw file bytes; kind/mime are inferred from the extension. */
  importFile(fileName: string, data: ArrayBuffer): AssetRecord {
    const ext = fileName.split('.').pop()!.toLowerCase();
    const info = MIME_BY_EXT[ext];
    if (!info) throw new Error(`Unsupported asset type ".${ext}". Supported: ${Object.keys(MIME_BY_EXT).join(', ')}`);
    const name = fileName.replace(/^.*[\\/]/, '').replace(/\.[^.]+$/, '');
    if (info.text) {
      const text = new TextDecoder().decode(data);
      if (info.mime === 'application/x-forge-animation') validateAnimation(text);
      if (info.mime === 'model/gltf+json' && /"uri"\s*:\s*"(?!data:)/.test(text)) {
        throw new Error('This .gltf references external files. Please export it as a single .glb (or embed buffers) and import again.');
      }
      return this.add({ name, kind: info.kind, mime: info.mime, data: text, encoding: 'text' });
    }
    return this.add({ name, kind: info.kind, mime: info.mime, data: arrayBufferToBase64(data), encoding: 'base64' });
  }

  rename(id: string, name: string) {
    const a = this.assets.get(id);
    if (!a) return;
    a.name = name;
    this.changed.fire();
  }

  remove(id: string) { if (this.assets.delete(id)) this.changed.fire(); }

  toJSON(): AssetRecord[] { return [...this.assets.values()]; }
}

export function assetBytes(a: AssetRecord): ArrayBuffer {
  return a.encoding === 'base64' ? base64ToArrayBuffer(a.data) : (new TextEncoder().encode(a.data).buffer as ArrayBuffer);
}

export function assetDataUrl(a: AssetRecord): string {
  return a.encoding === 'base64' ? `data:${a.mime};base64,${a.data}` : `data:${a.mime};charset=utf-8,${encodeURIComponent(a.data)}`;
}

// ---- Animations -------------------------------------------------------------

export interface AnimationKeyframe { t: number; position?: [number, number, number]; rotation?: [number, number, number]; scale?: number }
export interface AnimationClip { duration: number; easing?: 'linear' | 'smooth'; keyframes: AnimationKeyframe[] }

export function validateAnimation(text: string): AnimationClip {
  let a: any;
  try { a = JSON.parse(text); } catch { throw new Error('Animation file is not valid JSON.'); }
  if (!a || typeof a.duration !== 'number' || !Array.isArray(a.keyframes) || a.keyframes.length < 1) {
    throw new Error('Animation JSON needs { "duration": number, "keyframes": [{ "t": number, "position"?: [x,y,z], "rotation"?: [x,y,z], "scale"?: number }] }');
  }
  for (const k of a.keyframes) if (typeof k.t !== 'number') throw new Error('Every keyframe needs a numeric "t".');
  a.keyframes.sort((x: AnimationKeyframe, y: AnimationKeyframe) => x.t - y.t);
  return a as AnimationClip;
}

/** Samples an animation clip at time t (seconds). */
export function sampleAnimation(clip: AnimationClip, t: number) {
  const ks = clip.keyframes;
  const pick = (k: AnimationKeyframe, f: 'position' | 'rotation') => k[f] ?? [0, 0, 0];
  if (t <= ks[0].t) return { position: pick(ks[0], 'position'), rotation: pick(ks[0], 'rotation'), scale: ks[0].scale ?? 1 };
  for (let i = 0; i < ks.length - 1; i++) {
    const a = ks[i], b = ks[i + 1];
    if (t >= a.t && t <= b.t) {
      let f = b.t === a.t ? 1 : (t - a.t) / (b.t - a.t);
      if (clip.easing !== 'linear') f = f * f * (3 - 2 * f);
      const lerp3 = (p: number[], q: number[]) => p.map((v, j) => v + (q[j] - v) * f) as [number, number, number];
      return {
        position: lerp3(pick(a, 'position'), pick(b, 'position')),
        rotation: lerp3(pick(a, 'rotation'), pick(b, 'rotation')),
        scale: (a.scale ?? 1) + ((b.scale ?? 1) - (a.scale ?? 1)) * f,
      };
    }
  }
  const last = ks[ks.length - 1];
  return { position: pick(last, 'position'), rotation: pick(last, 'rotation'), scale: last.scale ?? 1 };
}

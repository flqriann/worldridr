// Web Audio playback for Sound instances (positional when parented to a part).

import type { Instance } from '../core/Instance';
import type { AssetStore } from '../assets/AssetStore';
import { assetBytes } from '../assets/AssetStore';
import type { Vec3 } from '../core/types';

interface Playback { source: AudioBufferSourceNode; gain: GainNode; panner: PannerNode | null; startedAt: number; offset: number; ended: boolean }

let ctx: AudioContext | null = null;
export function audioContext(): AudioContext {
  if (!ctx) ctx = new AudioContext();
  if (ctx.state === 'suspended') ctx.resume().catch(() => {});
  return ctx;
}

const bufferCache = new Map<string, Promise<AudioBuffer>>();
export function decodeSound(assets: AssetStore, id: string): Promise<AudioBuffer> {
  const a = assets.get(id);
  if (!a || a.kind !== 'sound') return Promise.reject(new Error(`Sound asset "${id}" not found`));
  const key = a.id + ':' + a.data.length;
  let p = bufferCache.get(key);
  if (!p) {
    p = audioContext().decodeAudioData(assetBytes(a).slice(0));
    bufferCache.set(key, p);
    p.catch(() => bufferCache.delete(key));
  }
  return p;
}

/** One-shot preview used by the asset browser. Returns a stop function. */
export async function previewSound(assets: AssetStore, id: string): Promise<() => void> {
  const buf = await decodeSound(assets, id);
  const c = audioContext();
  const src = c.createBufferSource();
  src.buffer = buf;
  src.connect(c.destination);
  src.start();
  return () => { try { src.stop(); } catch { /* already stopped */ } };
}

export class AudioSystem {
  private playing = new Map<Instance, Playback>();
  private paused = new Map<Instance, number>();
  masterVolume = 1;
  onEnded: (s: Instance) => void = () => {};
  onError: (msg: string) => void = () => {};
  private globalPause = false;

  constructor(private assets: AssetStore) {}

  async play(s: Instance, fromOffset = 0) {
    this.stop(s, false);
    const id = s.props.SoundId as string;
    if (!id) { this.onError(`${s.getFullName()}: SoundId is empty`); return; }
    let buf: AudioBuffer;
    try { buf = await decodeSound(this.assets, id); } catch (e) { this.onError(`${s.getFullName()}: ${(e as Error).message}`); return; }
    if (!s.props.Playing || !s.dm) return; // stopped while loading
    const c = audioContext();
    const source = c.createBufferSource();
    source.buffer = buf;
    source.loop = !!s.props.Looped;
    source.playbackRate.value = s.props.PlaybackSpeed as number;
    const gain = c.createGain();
    gain.gain.value = (s.props.Volume as number) * this.masterVolume;
    let panner: PannerNode | null = null;
    if (s.parent?.isA('BasePart')) {
      panner = c.createPanner();
      panner.panningModel = 'HRTF';
      panner.distanceModel = 'linear';
      panner.refDistance = 4;
      panner.maxDistance = s.props.RollOffMaxDistance as number;
      source.connect(gain).connect(panner).connect(c.destination);
    } else source.connect(gain).connect(c.destination);
    const pb: Playback = { source, gain, panner, startedAt: c.currentTime, offset: fromOffset, ended: false };
    source.onended = () => {
      if (this.playing.get(s) !== pb || pb.ended) return;
      pb.ended = true;
      this.playing.delete(s);
      if (s.dm) s.setSilent('Playing', false);
      this.onEnded(s);
    };
    source.start(0, fromOffset % buf.duration);
    this.playing.set(s, pb);
    if (this.globalPause) c.suspend();
  }

  stop(s: Instance, resetFlag = true) {
    const pb = this.playing.get(s);
    this.paused.delete(s);
    if (!pb) return;
    pb.ended = true;
    this.playing.delete(s);
    try { pb.source.stop(); } catch { /* not started */ }
    if (resetFlag && s.dm && s.props.Playing) s.setSilent('Playing', false);
  }

  pause(s: Instance) {
    const pb = this.playing.get(s);
    if (!pb) return;
    const c = audioContext();
    const pos = pb.offset + (c.currentTime - pb.startedAt) * (s.props.PlaybackSpeed as number);
    this.stop(s);
    this.paused.set(s, pos);
  }

  resume(s: Instance) {
    const pos = this.paused.get(s) ?? 0;
    this.paused.delete(s);
    s.setSilent('Playing', true);
    this.play(s, pos);
  }

  isPlaying(s: Instance) { return this.playing.has(s); }

  /** Updates live properties (volume/speed/loop) after a property change. */
  refresh(s: Instance) {
    const pb = this.playing.get(s);
    if (!pb) return;
    pb.gain.gain.value = (s.props.Volume as number) * this.masterVolume;
    pb.source.playbackRate.value = s.props.PlaybackSpeed as number;
    pb.source.loop = !!s.props.Looped;
    if (pb.panner) pb.panner.maxDistance = s.props.RollOffMaxDistance as number;
  }

  /** Positions panners and the listener. */
  update(listenerPos: Vec3, forward: Vec3) {
    if (!ctx) return;
    const l = ctx.listener;
    if (l.positionX) {
      l.positionX.value = listenerPos.x; l.positionY.value = listenerPos.y; l.positionZ.value = listenerPos.z;
      l.forwardX.value = forward.x; l.forwardY.value = forward.y; l.forwardZ.value = forward.z;
      l.upX.value = 0; l.upY.value = 1; l.upZ.value = 0;
    }
    for (const [s, pb] of this.playing) {
      if (!pb.panner || !s.parent?.isA('BasePart')) continue;
      const p = s.parent.props.Position as Vec3;
      pb.panner.positionX.value = p.x; pb.panner.positionY.value = p.y; pb.panner.positionZ.value = p.z;
    }
  }

  setPaused(p: boolean) {
    this.globalPause = p;
    if (!ctx) return;
    if (p) ctx.suspend().catch(() => {}); else ctx.resume().catch(() => {});
  }

  dispose() {
    for (const s of [...this.playing.keys()]) this.stop(s, false);
    this.paused.clear();
    if (ctx && ctx.state === 'suspended') ctx.resume().catch(() => {});
  }
}

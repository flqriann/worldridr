import { nextFrame } from '../core/frame';
import { useEffect, useReducer, useRef } from 'react';
import type { Signal } from '../core/Signal';
import type { Studio } from '../editor/Studio';
import type { DataModel, Instance } from '../core/Instance';
import type { Selection } from '../editor/Selection';
import type { AssetStore } from '../assets/AssetStore';

/** Re-renders the component when any of the given signals fire (coalesced per animation frame). */
export function useSignals(signals: (Signal<any> | null | undefined)[], throttle = true) {
  const [, force] = useReducer((x: number) => x + 1, 0);
  const pending = useRef(false);
  useEffect(() => {
    const fire = () => {
      if (!throttle) { force(); return; }
      if (pending.current) return;
      pending.current = true;
      nextFrame(() => { pending.current = false; force(); });
    };
    const offs = signals.filter(Boolean).map((s) => s!.connect(fire));
    return () => offs.forEach((o) => o());
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, signals);
}

/** What the inspector panels currently look at: the edit document or the running test. */
export interface SceneView {
  dm: DataModel;
  selection: Selection;
  assets: AssetStore;
  live: boolean;
  /** Applies a mutation (with undo in edit mode). */
  mutate(label: string, fn: () => void, coalesce?: string): void;
}

export function sceneView(studio: Studio): SceneView | null {
  const play = studio.play;
  if (play && play.dm) {
    return { dm: play.dm, selection: play.selection, assets: play.assets, live: true, mutate: (_l, fn) => { try { fn(); } catch (e) { studio.output.error((e as Error).message); } } };
  }
  const d = studio.active;
  if (!d) return null;
  return {
    dm: d.dm, selection: d.selection, assets: d.assets, live: false,
    mutate: (label, fn, coalesce) => { try { d.transact(label, fn, coalesce); } catch (e) { studio.output.error((e as Error).message); } },
  };
}

export function isEditable(inst: Instance) { return !inst.locked; }

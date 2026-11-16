// Undo/redo. Every mutation of an attached instance reports a HistoryOp; ops
// are grouped into entries by transactions (doc.transact). Undo replays the
// inverse ops with recording suspended.

import { Signal } from './Signal';
import type { HistoryOp, Instance } from './Instance';
import { valueEqual } from './types';

export interface HistoryEntry {
  label: string;
  ops: HistoryOp[];
  time: number;
  coalesceKey?: string;
}

const MAX_ENTRIES = 300;

export class History {
  private undoStack: HistoryEntry[] = [];
  private redoStack: HistoryEntry[] = [];
  private open: HistoryEntry | null = null;
  private depth = 0;
  private suspended = 0;
  /** Number of entries pushed since the last save marker (used for dirty state). */
  private version = 0;
  private savedVersion = 0;
  readonly changed = new Signal();
  /** Applies terrain snapshots (set by the terrain module to avoid a dependency cycle). */
  static applyTerrain: (inst: Instance, snapshot: unknown) => void = () => {};

  get canUndo() { return this.undoStack.length > 0; }
  get canRedo() { return this.redoStack.length > 0; }
  get undoLabel() { return this.undoStack.at(-1)?.label; }
  get redoLabel() { return this.redoStack.at(-1)?.label; }
  get isDirty() { return this.version !== this.savedVersion; }
  markSaved() { this.savedVersion = this.version; this.changed.fire(); }
  /** Forces dirty state (e.g. asset list changed outside history). */
  touch() { this.version++; this.changed.fire(); }

  begin(label: string, coalesceKey?: string) {
    if (this.depth++ === 0) this.open = { label, ops: [], time: Date.now(), coalesceKey };
  }

  end() {
    if (this.depth === 0) return;
    if (--this.depth > 0) return;
    const e = this.open!;
    this.open = null;
    if (e.ops.length === 0) return;
    const top = this.undoStack.at(-1);
    if (e.coalesceKey && top && top.coalesceKey === e.coalesceKey && e.time - top.time < 1500) {
      // merge consecutive edits (e.g. typing in a script) into one undo step
      for (const op of e.ops) this.mergeInto(top, op);
      top.time = e.time;
    } else {
      this.undoStack.push(e);
      if (this.undoStack.length > MAX_ENTRIES) this.undoStack.shift();
    }
    this.redoStack = [];
    this.version++;
    this.changed.fire();
  }

  transact<T>(label: string, fn: () => T, coalesceKey?: string): T {
    this.begin(label, coalesceKey);
    try { return fn(); } finally { this.end(); }
  }

  record(op: HistoryOp) {
    if (this.suspended > 0) return;
    if (this.open) { this.open.ops.push(op); return; }
    // Mutation outside a transaction: becomes its own entry.
    this.begin(op.k === 'prop' ? `Change ${op.prop}` : 'Edit');
    this.open!.ops.push(op);
    this.end();
  }

  private mergeInto(entry: HistoryEntry, op: HistoryOp) {
    if (op.k === 'prop') {
      const ex = entry.ops.find((o) => o.k === 'prop' && o.inst === op.inst && o.prop === op.prop);
      if (ex && ex.k === 'prop') { ex.after = op.after; return; }
    }
    entry.ops.push(op);
  }

  suspend<T>(fn: () => T): T {
    this.suspended++;
    try { return fn(); } finally { this.suspended--; }
  }

  undo(): string | null {
    if (this.open) return null;
    const e = this.undoStack.pop();
    if (!e) return null;
    this.suspend(() => { for (let i = e.ops.length - 1; i >= 0; i--) this.apply(e.ops[i], true); });
    this.redoStack.push(e);
    this.version--;
    this.changed.fire();
    return e.label;
  }

  redo(): string | null {
    if (this.open) return null;
    const e = this.redoStack.pop();
    if (!e) return null;
    this.suspend(() => { for (const op of e.ops) this.apply(op, false); });
    this.undoStack.push(e);
    this.version++;
    this.changed.fire();
    return e.label;
  }

  clear() { this.undoStack = []; this.redoStack = []; this.changed.fire(); }

  private apply(op: HistoryOp, inverse: boolean) {
    switch (op.k) {
      case 'prop': {
        const v = inverse ? op.before : op.after;
        if (!valueEqual(op.inst.props[op.prop], v)) op.inst.setSilent(op.prop, v);
        break;
      }
      case 'attr':
        op.inst.setAttribute(op.key, inverse ? op.before : op.after);
        break;
      case 'parent':
        if (inverse) op.inst.setParent(op.beforeParent, op.beforeIndex < 0 ? undefined : op.beforeIndex);
        else op.inst.setParent(op.afterParent, op.afterIndex < 0 ? undefined : op.afterIndex);
        break;
      case 'terrain':
        History.applyTerrain(op.inst, inverse ? op.before : op.after);
        break;
    }
  }
}

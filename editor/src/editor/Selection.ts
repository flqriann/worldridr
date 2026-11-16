import { Signal } from '../core/Signal';
import type { DataModel, Instance } from '../core/Instance';

/** Ordered selection; the last item is the "primary" selection. */
export class Selection {
  private items: Instance[] = [];
  readonly changed = new Signal();
  private unsub: (() => void) | null = null;

  bind(dm: DataModel) {
    this.unsub?.();
    this.unsub = dm.removed.connect((inst) => {
      if (this.items.includes(inst)) { this.items = this.items.filter((i) => i !== inst); this.changed.fire(); }
    });
  }

  get list(): readonly Instance[] { return this.items; }
  get primary(): Instance | undefined { return this.items.at(-1); }
  get size() { return this.items.length; }
  has(i: Instance) { return this.items.includes(i); }

  set(list: Instance[]) {
    const uniq = [...new Set(list)];
    if (uniq.length === this.items.length && uniq.every((x, k) => x === this.items[k])) return;
    this.items = uniq;
    this.changed.fire();
  }
  add(i: Instance) { if (!this.has(i)) { this.items = [...this.items, i]; this.changed.fire(); } }
  toggle(i: Instance) { this.has(i) ? this.set(this.items.filter((x) => x !== i)) : this.add(i); }
  clear() { this.set([]); }
}

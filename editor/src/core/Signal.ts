/** Minimal typed event emitter used across the engine. */
export class Signal<A extends unknown[] = []> {
  private listeners = new Set<(...args: A) => void>();

  connect(fn: (...args: A) => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  fire(...args: A) {
    for (const fn of [...this.listeners]) {
      try { fn(...args); } catch (e) { console.error('Signal listener failed', e); }
    }
  }

  clear() { this.listeners.clear(); }
  get size() { return this.listeners.size; }
}

import { nextFrame } from '../core/frame';
import { Signal } from '../core/Signal';

export type LogLevel = 'info' | 'warn' | 'error' | 'print' | 'system';

export interface LogEntry {
  id: number;
  time: number;
  level: LogLevel;
  message: string;
  /** Script instance id + line for clickable errors. */
  source?: { scriptId: string; line?: number; name?: string };
  count: number;
}

/** The Output panel's log store. */
export class OutputLog {
  entries: LogEntry[] = [];
  readonly changed = new Signal();
  private nextId = 1;
  private pending = false;

  log(level: LogLevel, message: string, source?: LogEntry['source']) {
    const last = this.entries.at(-1);
    if (last && last.level === level && last.message === message && last.source?.scriptId === source?.scriptId && last.source?.line === source?.line) {
      last.count++;
      last.time = Date.now();
    } else {
      this.entries.push({ id: this.nextId++, time: Date.now(), level, message, source, count: 1 });
      if (this.entries.length > 2000) this.entries.splice(0, this.entries.length - 2000);
    }
    if (level === 'error') console.warn('[Output]', message);
    if (!this.pending) {
      this.pending = true;
      nextFrame(() => { this.pending = false; this.changed.fire(); });
    }
  }

  info(m: string) { this.log('info', m); }
  warn(m: string, s?: LogEntry['source']) { this.log('warn', m, s); }
  error(m: string, s?: LogEntry['source']) { this.log('error', m, s); }
  system(m: string) { this.log('system', m); }

  clear() { this.entries = []; this.changed.fire(); }

  get errorCount() { return this.entries.reduce((n, e) => n + (e.level === 'error' ? e.count : 0), 0); }
  get warnCount() { return this.entries.reduce((n, e) => n + (e.level === 'warn' ? e.count : 0), 0); }
}

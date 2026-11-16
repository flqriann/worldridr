// WebSocket connection to a game session on the Forge Server.

import { Signal } from '../core/Signal';
import type { ServerApi } from './api';

export interface RosterEntry { id: number; name: string }
export interface Welcome { you: number; token: string; host: number | null; players: RosterEntry[]; game: string; version: number }

export type NetMessage = { t: string; from?: number; [k: string]: unknown };

export class NetClient {
  private ws: WebSocket | null = null;
  welcome!: Welcome;
  players = new Map<number, string>();
  host: number | null = null;
  rtt = 0;
  closedReason: string | null = null;
  private pingTimer: number | undefined;

  readonly joined = new Signal<[RosterEntry]>();
  readonly left = new Signal<[number]>();
  readonly hostChanged = new Signal<[number | null]>();
  readonly message = new Signal<[NetMessage]>();
  readonly chat = new Signal<[{ from: number; name: string; text: string }]>();
  readonly closed = new Signal<[string]>();

  constructor(readonly api: ServerApi, readonly gameId: string, readonly name: string, readonly version?: number) {}

  get id() { return this.welcome.you; }
  get isHost() { return this.host === this.welcome?.you; }
  get connected() { return this.ws?.readyState === WebSocket.OPEN; }

  connect(timeoutMs = 8000): Promise<Welcome> {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(this.api.wsUrl(this.gameId, this.name, this.version));
      this.ws = ws;
      const timer = setTimeout(() => { reject(new Error('Connection timed out.')); ws.close(); }, timeoutMs);
      let welcomed = false;
      ws.onmessage = (e) => {
        let m: NetMessage;
        try { m = JSON.parse(String(e.data)); } catch { return; }
        if (m.t === 'welcome') {
          clearTimeout(timer);
          welcomed = true;
          this.welcome = m as unknown as Welcome;
          this.host = this.welcome.host;
          for (const p of this.welcome.players) this.players.set(p.id, p.name);
          this.pingTimer = window.setInterval(() => this.send({ t: 'ping', ts: performance.now() }), 2000);
          resolve(this.welcome);
          return;
        }
        if (m.t === 'error') { if (!welcomed) { clearTimeout(timer); reject(new Error(String(m.message))); } this.closedReason = String(m.message); return; }
        this.dispatch(m);
      };
      ws.onerror = () => { if (!welcomed) { clearTimeout(timer); reject(new Error('Could not connect to the game server.')); } };
      ws.onclose = () => {
        clearTimeout(timer);
        clearInterval(this.pingTimer);
        if (!welcomed) reject(new Error(this.closedReason ?? 'The server closed the connection.'));
        this.closed.fire(this.closedReason ?? 'Disconnected from the server.');
      };
    });
  }

  private dispatch(m: NetMessage) {
    switch (m.t) {
      case 'joined': this.players.set(m.id as number, m.name as string); this.joined.fire({ id: m.id as number, name: m.name as string }); break;
      case 'left': this.players.delete(m.id as number); this.left.fire(m.id as number); break;
      case 'host': this.host = m.id as number; this.hostChanged.fire(this.host); break;
      case 'chat': this.chat.fire({ from: m.from as number, name: m.name as string, text: m.text as string }); break;
      case 'pong': this.rtt = Math.round(performance.now() - Number(m.ts)); break;
      default: this.message.fire(m);
    }
  }

  send(m: NetMessage) {
    if (this.ws?.readyState === WebSocket.OPEN) this.ws.send(JSON.stringify(m));
  }

  close() {
    clearInterval(this.pingTimer);
    this.ws?.close();
  }
}

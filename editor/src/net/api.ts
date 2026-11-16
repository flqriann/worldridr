// REST client for the Forge Server (server/src/main.rs).

import { t } from '../i18n';

export interface GameInfo {
  id: string;
  name: string;
  description: string;
  author: string;
  created: number;
  updated: number;
  live: number | null;
  thumbnail?: string | null;
  versions?: number;
  playersOnline?: number;
}
export interface VersionInfo { number: number; hash: string; parent: number | null; message: string; author: string; created: number; size: number }
export interface GameDetails extends GameInfo { history: VersionInfo[]; sessions: { version: number; players: { id: number; name: string }[]; host: number | null }[] }

export class ApiError extends Error {
  constructor(message: string, readonly status: number) { super(message); }
}

export function normalizeServerUrl(url: string): string {
  let u = url.trim();
  if (!/^https?:\/\//i.test(u)) u = 'http://' + u;
  return u.replace(/\/+$/, '');
}

export class ServerApi {
  readonly base: string;
  constructor(base: string, private publishKey = '') { this.base = normalizeServerUrl(base); }

  private async req<T>(path: string, init: RequestInit = {}, auth = false): Promise<T> {
    const headers: Record<string, string> = { ...(init.headers as Record<string, string>) };
    if (init.body) headers['content-type'] = 'application/json';
    if (auth) headers['x-forge-key'] = this.publishKey;
    let res: Response;
    try {
      res = await fetch(this.base + path, { ...init, headers });
    } catch {
      throw new ApiError(t('Cannot reach the server at {url}. Is it running?', { url: this.base }), 0);
    }
    if (!res.ok) {
      const body = await res.json().catch(() => null) as { error?: string } | null;
      throw new ApiError(body?.error ?? `${res.status} ${res.statusText}`, res.status);
    }
    return res.headers.get('content-type')?.includes('json') ? res.json() as Promise<T> : (res.text() as unknown as Promise<T>);
  }

  health() { return this.req<{ name: string; version: string; games: number }>('/api/health'); }
  games() { return this.req<{ games: GameInfo[] }>('/api/games').then((r) => r.games); }
  game(id: string) { return this.req<GameDetails>(`/api/games/${encodeURIComponent(id)}`); }
  createGame(name: string, description: string, author: string) {
    return this.req<GameInfo>('/api/games', { method: 'POST', body: JSON.stringify({ name, description, author }) }, true);
  }
  updateGame(id: string, patch: { name?: string; description?: string }) {
    return this.req<GameInfo>(`/api/games/${encodeURIComponent(id)}`, { method: 'PATCH', body: JSON.stringify(patch) }, true);
  }
  publish(id: string, body: { parent: number | null; message: string; author: string; project: string; thumbnail?: string | null; live?: boolean; force?: boolean }) {
    return this.req<VersionInfo>(`/api/games/${encodeURIComponent(id)}/versions`, { method: 'POST', body: JSON.stringify(body) }, true);
  }
  setLive(id: string, version: number) {
    return this.req<GameInfo>(`/api/games/${encodeURIComponent(id)}/live`, { method: 'POST', body: JSON.stringify({ version }) }, true);
  }
  /** Project file text of the live version (or a specific version). */
  async project(id: string, version?: number): Promise<{ text: string; version: number }> {
    const path = version ? `/api/games/${encodeURIComponent(id)}/versions/${version}` : `/api/games/${encodeURIComponent(id)}/live`;
    let res: Response;
    try { res = await fetch(this.base + path); } catch { throw new ApiError(t('Cannot reach the server at {url}. Is it running?', { url: this.base }), 0); }
    if (!res.ok) { const b = await res.json().catch(() => null) as { error?: string } | null; throw new ApiError(b?.error ?? res.statusText, res.status); }
    return { text: await res.text(), version: Number(res.headers.get('x-forge-version') ?? version ?? 0) };
  }
  dsGet(game: string, store: string, key: string) {
    return this.req<{ value: unknown }>(`/api/games/${encodeURIComponent(game)}/datastore/${encodeURIComponent(store)}/${encodeURIComponent(key)}`).then((r) => r.value ?? null);
  }
  dsSet(game: string, store: string, key: string, value: unknown, sessionToken: string) {
    return this.req<{ previous: unknown }>(`/api/games/${encodeURIComponent(game)}/datastore/${encodeURIComponent(store)}/${encodeURIComponent(key)}`,
      { method: 'PUT', body: JSON.stringify({ value }), headers: { 'x-forge-session': sessionToken } });
  }
  dsRemove(game: string, store: string, key: string, sessionToken: string) {
    return this.req<{ previous: unknown }>(`/api/games/${encodeURIComponent(game)}/datastore/${encodeURIComponent(store)}/${encodeURIComponent(key)}`,
      { method: 'DELETE', headers: { 'x-forge-session': sessionToken } });
  }
  wsUrl(id: string, name: string, version?: number) {
    const u = new URL(this.base.replace(/^http/, 'ws') + `/ws/${encodeURIComponent(id)}`);
    u.searchParams.set('name', name);
    if (version) u.searchParams.set('version', String(version));
    return u.toString();
  }
}

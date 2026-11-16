// HTTP for the map services. In the desktop app the main process performs the
// request (allow-listed hosts, proper User-Agent); in a plain browser (dev) fetch is used.

import { native } from '../project/ProjectIO';

export async function httpText(url: string, body?: string): Promise<string> {
  if (native) return (await native.netRequest({ url, method: body ? 'POST' : 'GET', body })) as string;
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), 50000);
  try {
    const res = await fetch(url, { signal: ctl.signal, ...(body ? { method: 'POST', body, headers: { 'Content-Type': 'application/x-www-form-urlencoded' } } : {}) });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return await res.text();
  } finally { clearTimeout(timer); }
}

export async function httpBinary(url: string): Promise<ArrayBuffer> {
  if (native) return (await native.netRequest({ url, binary: true })) as ArrayBuffer;
  const res = await fetch(url);
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return res.arrayBuffer();
}

export async function httpJson<T>(url: string, body?: string): Promise<T> {
  return JSON.parse(await httpText(url, body)) as T;
}

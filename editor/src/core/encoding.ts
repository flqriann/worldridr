// Binary <-> base64 helpers used by the project format.

export function bytesToBase64(bytes: Uint8Array): string {
  let s = '';
  const CHUNK = 0x8000;
  for (let i = 0; i < bytes.length; i += CHUNK) s += String.fromCharCode(...bytes.subarray(i, i + CHUNK));
  return btoa(s);
}

export function base64ToBytes(b64: string): Uint8Array {
  const s = atob(b64);
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
  return out;
}

export function arrayBufferToBase64(buf: ArrayBuffer): string { return bytesToBase64(new Uint8Array(buf)); }

export function base64ToArrayBuffer(b64: string): ArrayBuffer {
  const b = base64ToBytes(b64);
  return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength) as ArrayBuffer;
}

export function dataUrlToArrayBuffer(url: string): ArrayBuffer {
  return base64ToArrayBuffer(url.slice(url.indexOf(',') + 1));
}

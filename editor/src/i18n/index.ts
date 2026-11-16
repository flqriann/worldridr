// Localization. English source strings are the keys; language packs map
// them to translations. Built-in packs: English (default) and German. Users
// can add packs (JSON, see docs/LANGUAGE_PACKS.md) in Settings ▸ Language.

import { Signal } from '../core/Signal';
import { DE } from './de';

export interface LanguagePack {
  format: 'forge-language-pack';
  version: 1;
  code: string;
  name: string;
  author?: string;
  strings: Record<string, string>;
  /** true for packs shipped with the app */
  builtin?: boolean;
  /** 'folder' = loaded from the desktop languages folder (not copied into settings) */
  source?: 'folder' | 'user';
}

const EN: LanguagePack = { format: 'forge-language-pack', version: 1, code: 'en', name: 'English', strings: {}, builtin: true };
const GERMAN: LanguagePack = { format: 'forge-language-pack', version: 1, code: 'de', name: 'Deutsch', strings: DE, builtin: true };

const packs = new Map<string, LanguagePack>([[EN.code, EN], [GERMAN.code, GERMAN]]);
const LANG_KEY = 'forge.lang';
const PACKS_KEY = 'forge.languagePacks';
let current = 'en';
try { current = localStorage.getItem(LANG_KEY) ?? 'en'; } catch { /* default */ }

/** Fired after the language or the set of packs changed. */
export const languageChanged = new Signal();

/** Translates `key` (an English UI string). `{name}` placeholders are filled from `vars`. */
export function t(key: string, vars?: Record<string, string | number>): string {
  const pack = packs.get(current);
  let s = (pack && pack.strings[key]) || key;
  if (vars) s = s.replace(/\{(\w+)\}/g, (m, k) => (k in vars ? String(vars[k]) : m));
  return s;
}

export function currentLanguage() { return packs.has(current) ? current : 'en'; }
export function languages(): LanguagePack[] { return [...packs.values()]; }

export function setLanguage(code: string) {
  if (!packs.has(code)) return;
  current = code;
  try { localStorage.setItem(LANG_KEY, code); } catch { /* ignore */ }
  document.documentElement.lang = code;
  languageChanged.fire();
}

/** Validates and parses a language pack file. Throws readable errors. */
export function parseLanguagePack(text: string): LanguagePack {
  let raw: any;
  try { raw = JSON.parse(text); } catch { throw new Error(t('The language pack is not valid JSON.')); }
  if (!raw || raw.format !== 'forge-language-pack') throw new Error(t('Not a Forge language pack (format must be "forge-language-pack").'));
  if (typeof raw.code !== 'string' || !/^[a-z]{2,3}(-[A-Za-z0-9]{2,8})?$/.test(raw.code)) throw new Error(t('The pack needs a language code like "fr" or "pt-BR".'));
  if (typeof raw.name !== 'string' || !raw.name.trim()) throw new Error(t('The pack needs a display name.'));
  if (!raw.strings || typeof raw.strings !== 'object') throw new Error(t('The pack has no "strings" object.'));
  const strings: Record<string, string> = {};
  for (const [k, v] of Object.entries(raw.strings)) if (typeof v === 'string' && v.trim()) strings[k] = v;
  return { format: 'forge-language-pack', version: 1, code: raw.code, name: raw.name.trim(), author: typeof raw.author === 'string' ? raw.author : undefined, strings };
}

/** Adds (or replaces) a user language pack and remembers it. */
export function installLanguagePack(pack: LanguagePack) {
  if (packs.get(pack.code)?.builtin) throw new Error(t('"{code}" is a built-in language and cannot be replaced. Use another code (e.g. "{code}-custom").', { code: pack.code }));
  packs.set(pack.code, pack);
  saveUserPacks();
  languageChanged.fire();
}

/** Registers a pack for this session only (e.g. from the languages folder). */
export function registerLanguagePack(pack: LanguagePack) {
  if (packs.get(pack.code)?.builtin) return;
  packs.set(pack.code, { ...pack, builtin: false, source: 'folder' });
  if (pack.code === current) { document.documentElement.lang = current; }
  languageChanged.fire();
}

export function removeLanguagePack(code: string) {
  if (packs.get(code)?.builtin) return;
  packs.delete(code);
  if (current === code) setLanguage('en');
  saveUserPacks();
  languageChanged.fire();
}

function saveUserPacks() {
  const user = [...packs.values()].filter((p) => !p.builtin && p.source !== 'folder');
  try { localStorage.setItem(PACKS_KEY, JSON.stringify(user)); } catch { /* storage full */ }
}

function loadUserPacks() {
  try {
    const list = JSON.parse(localStorage.getItem(PACKS_KEY) ?? '[]') as unknown[];
    for (const p of list) {
      try { const pack = parseLanguagePack(JSON.stringify(p)); if (!packs.get(pack.code)?.builtin) packs.set(pack.code, pack); } catch { /* skip broken pack */ }
    }
  } catch { /* none */ }
}
loadUserPacks();

/** A template for translators: every known UI string, prefilled with the German or English text. */
export function languagePackTemplate(prefill: 'en' | 'de' = 'en'): string {
  const strings: Record<string, string> = {};
  for (const k of Object.keys(DE).sort()) strings[k] = prefill === 'de' ? DE[k] : k;
  return JSON.stringify({ format: 'forge-language-pack', version: 1, code: 'xx', name: 'My language', author: '', strings }, null, 2);
}

/** Number of template strings a pack translates (for the settings list). */
export function packCoverage(p: LanguagePack): number {
  if (p.code === 'en') return 1;
  const keys = Object.keys(DE);
  return keys.filter((k) => p.strings[k]).length / keys.length;
}

/** Marks a string for translation without translating it here (translate later with t()). */
export const msg = (s: string) => s;

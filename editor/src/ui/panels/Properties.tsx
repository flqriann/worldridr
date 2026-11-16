import { nextFrame } from '../../core/frame';
// Properties: shows the shared properties of the selection grouped by
// category with type-specific editors; edits apply immediately (with undo).

import { useEffect, useState } from 'react';
import type { Studio } from '../../editor/Studio';
import type { Instance } from '../../core/Instance';
import { getProps, type PropDef } from '../../core/classes';
import { formatNum, normalizeColor, udim2, v3, valueEqual, type UDim2, type Vec3 } from '../../core/types';
import { listBuiltin } from '../../assets/builtin';
import { ClassIcon, Icon } from '../icons';
import { sceneView, useSignals, type SceneView } from '../hooks';

import { t } from '../../i18n';
const CATEGORY_ORDER = ['Data', 'Appearance', 'Transform', 'Part', 'Behavior', 'Physics', 'Light', 'Emission', 'Motion', 'Sound', 'Animation', 'Script', 'Camera', 'Character', 'Avatar', 'Spawn', 'Text', 'Image', 'Layout', 'Water', 'Fog'];
const collapsed = new Set<string>();
const MIXED = Symbol('mixed');

export function Properties({ studio }: { studio: Studio }) {
  const view = sceneView(studio);
  const [filter, setFilter] = useState('');
  const [, setTick] = useState(0);
  useSignals([studio.changed, view?.selection.changed, view?.assets.changed]);
  const list = view?.selection.list ?? [];

  useEffect(() => {
    if (!view) return;
    let pending = false;
    const sel = new Set(list);
    const off = view.dm.changed.connect((i) => {
      if (!sel.has(i) || pending) return;
      pending = true;
      nextFrame(() => { pending = false; setTick((t) => t + 1); });
    });
    const off2 = view.dm.attributeChanged.connect((i) => { if (sel.has(i)) setTick((t) => t + 1); });
    const off3 = view.dm.transformsChanged.connect((moved) => {
      if (pending || !moved.some((m) => sel.has(m))) return;
      pending = true;
      nextFrame(() => { pending = false; setTick((t) => t + 1); });
    });
    return () => { off(); off2(); off3(); };
  }, [view?.dm, list]);

  if (!view) return <div className="panel-empty">{t('No project open')}</div>;
  if (!list.length) return <div className="panel-empty">{t('Select an object in the Explorer or viewport to see its properties.')}</div>;

  const first = list[0];
  // properties shared by all selected classes
  let defs = getProps(first.className);
  for (const i of list.slice(1)) { const names = new Set(getProps(i.className).map((d) => d.name)); defs = defs.filter((d) => names.has(d.name)); }
  defs = defs.filter((d) => !d.hidden || d.type === 'source');
  const q = filter.trim().toLowerCase();
  if (q) defs = defs.filter((d) => d.name.toLowerCase().includes(q));
  const groups = new Map<string, PropDef[]>();
  for (const d of defs) { const g = groups.get(d.category) ?? []; g.push(d); groups.set(d.category, g); }
  const cats = [...groups.keys()].sort((a, b) => (CATEGORY_ORDER.indexOf(a) + 100) % 200 - (CATEGORY_ORDER.indexOf(b) + 100) % 200);

  const valueOf = (prop: string): unknown => {
    const v = first.props[prop];
    return list.every((i) => valueEqual(i.props[prop], v)) ? v : MIXED;
  };
  const commit = (d: PropDef, value: unknown) => {
    const targets = list.filter((i) => !(d.name === 'Name' && i.locked));
    view.mutate(`Set ${d.name}`, () => targets.forEach((i) => i.set(d.name, value)));
  };

  return (
    <div className="properties">
      <div className="panel-toolbar">
        <div className="search-box">
          <Icon name="search" size={13} />
          <input placeholder={t('Filter properties')} value={filter} onChange={(e) => setFilter(e.target.value)} />
        </div>
      </div>
      <div className="props-title">
        <ClassIcon className={first.className} />
        <span>{list.length > 1 ? t('{n} objects', { n: list.length }) : `${first.className} "${first.name}"`}</span>
        {view.live && <span className="badge live">{t('LIVE')}</span>}
      </div>
      <div className="props-scroll">
        {cats.map((cat) => (
          <div key={cat} className="prop-group">
            <div className="prop-group-header" onClick={() => { collapsed.has(cat) ? collapsed.delete(cat) : collapsed.add(cat); setTick((t) => t + 1); }}>
              <Icon name={collapsed.has(cat) ? 'chevronRight' : 'chevronDown'} size={12} />{t(cat)}
            </div>
            {!collapsed.has(cat) && groups.get(cat)!.map((d) => (
              <div key={d.name} className="prop-row" title={d.doc ?? d.name}>
                <div className="prop-name">{d.name}</div>
                <div className="prop-value">
                  <PropEditor def={d} value={valueOf(d.name)} readOnly={d.readOnly || (d.name === 'Name' && first.locked)} onCommit={(v) => commit(d, v)} view={view} studio={studio} inst={first} />
                </div>
              </div>
            ))}
          </div>
        ))}
        {list.length === 1 && !q && <Attributes inst={first} view={view} studio={studio} />}
      </div>
    </div>
  );
}

function NumberField({ value, onCommit, step = 1, int = false, min, max, readOnly }: { value: number | typeof MIXED; onCommit: (v: number) => void; step?: number; int?: boolean; min?: number; max?: number; readOnly?: boolean }) {
  const shown = value === MIXED ? '' : formatNum(value);
  const [text, setText] = useState(shown);
  const [focused, setFocused] = useState(false);
  useEffect(() => { if (!focused) setText(shown); }, [shown, focused]);
  const commit = (raw: string) => {
    if (raw.trim() === '' && value === MIXED) return;
    let n: number;
    try { n = evalNumber(raw); } catch { setText(shown); return; }
    if (int) n = Math.round(n);
    if (min !== undefined) n = Math.max(min, n);
    if (max !== undefined) n = Math.min(max, n);
    if (value === MIXED || n !== value) onCommit(n);
    setText(formatNum(n));
  };
  return (
    <input className="num-input" value={text} placeholder={value === MIXED ? '—' : ''} readOnly={readOnly}
      onFocus={(e) => { setFocused(true); e.target.select(); }}
      onBlur={(e) => { setFocused(false); commit(e.target.value); }}
      onChange={(e) => setText(e.target.value)}
      onKeyDown={(e) => {
        if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
        if (e.key === 'Escape') { setText(shown); (e.target as HTMLInputElement).blur(); }
        if ((e.key === 'ArrowUp' || e.key === 'ArrowDown') && value !== MIXED) {
          e.preventDefault();
          const n = (value as number) + (e.key === 'ArrowUp' ? 1 : -1) * step * (e.shiftKey ? 10 : 1);
          commit(String(n));
        }
      }} />
  );
}

/** Accepts simple arithmetic like "4*2" or "10/3" in number fields. */
function evalNumber(s: string): number {
  const src = s.replace(/\s+/g, '');
  let i = 0;
  const num = (): number => {
    if (src[i] === '(') { i++; const v = expr(); if (src[i++] !== ')') throw new Error('invalid'); return v; }
    if (src[i] === '-' || src[i] === '+') { const neg = src[i++] === '-'; const v = num(); return neg ? -v : v; }
    const m = src.slice(i).match(/^(\d+\.?\d*|\.\d+)(e[+-]?\d+)?/i);
    if (!m) throw new Error('invalid');
    i += m[0].length;
    return parseFloat(m[0]);
  };
  const term = (): number => { let v = num(); while (src[i] === '*' || src[i] === '/') { const op = src[i++]; const r = num(); v = op === '*' ? v * r : v / r; } return v; };
  const expr = (): number => { let v = term(); while (src[i] === '+' || src[i] === '-') { const op = src[i++]; const r = term(); v = op === '+' ? v + r : v - r; } return v; };
  const n = expr();
  if (i !== src.length || !Number.isFinite(n)) throw new Error('invalid');
  return n;
}

function TextField({ value, onCommit, readOnly }: { value: string | typeof MIXED; onCommit: (v: string) => void; readOnly?: boolean }) {
  const shown = value === MIXED ? '' : value;
  const [text, setText] = useState(shown);
  const [focused, setFocused] = useState(false);
  useEffect(() => { if (!focused) setText(shown); }, [shown, focused]);
  return (
    <input className="text-input" value={text} placeholder={value === MIXED ? '—' : ''} readOnly={readOnly}
      onFocus={() => setFocused(true)}
      onBlur={(e) => { setFocused(false); if (e.target.value !== shown && !(value === MIXED && !e.target.value)) onCommit(e.target.value); }}
      onChange={(e) => setText(e.target.value)}
      onKeyDown={(e) => { if (e.key === 'Enter') (e.target as HTMLInputElement).blur(); if (e.key === 'Escape') { setText(shown); (e.target as HTMLInputElement).blur(); } }} />
  );
}

function ColorField({ value, onCommit, readOnly }: { value: string | typeof MIXED; onCommit: (v: string) => void; readOnly?: boolean }) {
  const hex = value === MIXED ? '#888888' : value;
  return (
    <div className="color-field">
      <label className="color-swatch" style={{ background: value === MIXED ? 'repeating-linear-gradient(45deg,#555 0 4px,#777 4px 8px)' : hex }}>
        <input type="color" value={hex} disabled={readOnly} onChange={(e) => onCommit(e.target.value)} />
      </label>
      <TextField value={value === MIXED ? MIXED : hexToRgbText(hex)} readOnly={readOnly} onCommit={(t) => { const c = normalizeColor(t); if (c) onCommit(c); }} />
    </div>
  );
}
function hexToRgbText(h: string) { const n = parseInt(h.slice(1), 16); return `${(n >> 16) & 255}, ${(n >> 8) & 255}, ${n & 255}`; }

function PropEditor({ def, value, readOnly, onCommit, view, studio, inst }: { def: PropDef; value: unknown; readOnly?: boolean; onCommit: (v: unknown) => void; view: SceneView; studio: Studio; inst: Instance }) {
  const mixed = value === MIXED;
  switch (def.type) {
    case 'bool':
      return <input type="checkbox" className={mixed ? 'mixed' : ''} checked={!mixed && !!value} disabled={readOnly} onChange={(e) => onCommit(e.target.checked)} />;
    case 'number': case 'int':
      return <NumberField value={value as number | typeof MIXED} int={def.type === 'int'} step={def.step ?? 1} min={def.min} max={def.max} readOnly={readOnly} onCommit={onCommit} />;
    case 'string':
      return <TextField value={value as string | typeof MIXED} readOnly={readOnly} onCommit={onCommit} />;
    case 'color':
      return <ColorField value={value as string | typeof MIXED} readOnly={readOnly} onCommit={onCommit} />;
    case 'enum':
      return (
        <select value={mixed ? '' : (value as string)} disabled={readOnly} onChange={(e) => onCommit(e.target.value)}>
          {mixed && <option value="">—</option>}
          {def.enumValues!.map((v) => <option key={v} value={v}>{v}</option>)}
        </select>
      );
    case 'vec3': {
      const v = mixed ? null : (value as Vec3);
      const set = (axis: 'x' | 'y' | 'z', n: number) => {
        if (v) onCommit({ ...v, [axis]: n });
        else view.mutate(`Set ${def.name}.${axis}`, () => view.selection.list.forEach((i) => { const cur = i.props[def.name] as Vec3; i.set(def.name, v3(axis === 'x' ? n : cur.x, axis === 'y' ? n : cur.y, axis === 'z' ? n : cur.z)); }));
      };
      return (
        <div className="vec3-field">
          {(['x', 'y', 'z'] as const).map((a) => (
            <label key={a} className={`axis axis-${a}`}><span>{a.toUpperCase()}</span>
              <NumberField value={v ? v[a] : MIXED} step={def.name === 'Orientation' ? 15 : 1} readOnly={readOnly} onCommit={(n) => set(a, n)} />
            </label>
          ))}
        </div>
      );
    }
    case 'udim2': {
      const u = mixed ? udim2() : (value as UDim2);
      const set = (k: keyof UDim2, n: number) => onCommit({ ...u, [k]: n });
      return (
        <div className="udim-field">
          <span className="udim-label">X</span>
          <NumberField value={u.xs} step={0.05} onCommit={(n) => set('xs', n)} /><NumberField value={u.xo} int onCommit={(n) => set('xo', n)} />
          <span className="udim-label">Y</span>
          <NumberField value={u.ys} step={0.05} onCommit={(n) => set('ys', n)} /><NumberField value={u.yo} int onCommit={(n) => set('yo', n)} />
        </div>
      );
    }
    case 'asset': {
      const kind = def.assetKind!;
      const options = [...view.assets.list(kind), ...listBuiltin(kind)];
      const cur = mixed ? '' : (value as string);
      const known = !cur || options.some((o) => o.id === cur);
      return (
        <select value={cur} disabled={readOnly} onChange={(e) => onCommit(e.target.value)}>
          <option value="">(none)</option>
          {!known && <option value={cur}>missing: {cur}</option>}
          <optgroup label={t('Project')}>{view.assets.list(kind).map((a) => <option key={a.id} value={a.id}>{a.name}</option>)}</optgroup>
          <optgroup label={t('Library')}>{listBuiltin(kind).map((a) => <option key={a.id} value={a.id}>{a.name}</option>)}</optgroup>
        </select>
      );
    }
    case 'source':
      return <button className="btn small" onClick={() => studio.openScript(inst)}><Icon name="script" size={12} /> {t('Edit script')}</button>;
  }
}

function Attributes({ inst, view, studio }: { inst: Instance; view: SceneView; studio: Studio }) {
  const entries = Object.entries(inst.attributes);
  const add = async () => {
    const name = await studio.prompt('Add attribute', 'Attribute name:', '');
    if (!name) return;
    if (!/^[A-Za-z_]\w{0,99}$/.test(name)) { studio.output.warn(t('Attribute names must start with a letter and contain only letters, digits and _.')); return; }
    view.mutate(t('Add attribute'), () => inst.setAttribute(name, ''));
  };
  return (
    <div className="prop-group">
      <div className="prop-group-header">{t('Attributes')}</div>
      {entries.map(([k, v]) => (
        <div key={k} className="prop-row">
          <div className="prop-name">{k}</div>
          <div className="prop-value attr-value">
            {typeof v === 'boolean' ? <input type="checkbox" checked={v} onChange={(e) => view.mutate('Set attribute', () => inst.setAttribute(k, e.target.checked))} />
              : typeof v === 'number' ? <NumberField value={v} onCommit={(n) => view.mutate('Set attribute', () => inst.setAttribute(k, n))} />
                : <TextField value={String(v)} onCommit={(t) => view.mutate('Set attribute', () => inst.setAttribute(k, /^-?\d+(\.\d+)?$/.test(t) ? Number(t) : t === 'true' ? true : t === 'false' ? false : t))} />}
            <button className="icon-btn" title={t('Remove attribute')} onClick={() => view.mutate('Remove attribute', () => inst.setAttribute(k, undefined))}><Icon name="close" size={11} /></button>
          </div>
        </div>
      ))}
      <div className="prop-row"><button className="btn small" onClick={add}><Icon name="plus" size={12} /> {t('Add attribute')}</button></div>
    </div>
  );
}

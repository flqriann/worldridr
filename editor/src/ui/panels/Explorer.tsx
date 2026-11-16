import { nextFrame } from '../../core/frame';
// Explorer: virtualized hierarchy tree with selection sync, rename,
// drag & drop re-parenting, search, context menus and quick insert.

import { useEffect, useMemo, useRef, useState } from 'react';
import type { Studio } from '../../editor/Studio';
import { insertMenuItems } from '../../editor/Studio';
import type { Instance } from '../../core/Instance';
import { ClassIcon, Icon } from '../icons';
import { sceneView, useSignals } from '../hooks';

import { t } from '../../i18n';
const ROW_H = 22;
const expandedByDm = new Map<string, Set<string>>();

interface Row { inst: Instance; depth: number; hasKids: boolean; open: boolean }

export function Explorer({ studio }: { studio: Studio }) {
  const view = sceneView(studio);
  const dm = view?.dm;
  const [, setTick] = useState(0);
  const [query, setQuery] = useState('');
  const [renaming, setRenaming] = useState<string | null>(null);
  const [scrollTop, setScrollTop] = useState(0);
  const [height, setHeight] = useState(400);
  const [dropHint, setDropHint] = useState<{ id: string; zone: 'before' | 'after' | 'into' } | null>(null);
  const listRef = useRef<HTMLDivElement>(null);
  const anchor = useRef<Instance | null>(null);
  const dragged = useRef<Instance[]>([]);
  useSignals([studio.changed, view?.selection.changed]);

  // structural changes only (ignore per-frame physics property changes)
  useEffect(() => {
    if (!dm) return;
    let pending = false;
    const bump = () => { if (!pending) { pending = true; nextFrame(() => { pending = false; setTick((t) => t + 1); }); } };
    const offs = [dm.added.connect(bump), dm.removed.connect(bump), dm.reparented.connect(bump), dm.changed.connect((_i, p) => { if (p === 'Name') bump(); })];
    return () => offs.forEach((o) => o());
  }, [dm]);

  const expanded = useMemo(() => {
    if (!dm) return new Set<string>();
    let s = expandedByDm.get(dm.id);
    if (!s) { s = new Set([dm.workspace.id]); expandedByDm.set(dm.id, s); }
    return s;
  }, [dm]);

  // reveal selected objects (e.g. picked in the viewport)
  const sel = view?.selection;
  useEffect(() => {
    if (!sel || !dm) return;
    return sel.changed.connect(() => {
      const p = sel.primary;
      if (!p || !p.dm) return;
      let a = p.parent;
      let changed = false;
      while (a && a !== dm) { if (!expanded.has(a.id)) { expanded.add(a.id); changed = true; } a = a.parent; }
      if (changed) setTick((t) => t + 1);
      requestAnimationFrame(() => {
        const idx = rowsRef.current.findIndex((r) => r.inst === p);
        const el = listRef.current;
        if (idx >= 0 && el) {
          const y = idx * ROW_H;
          if (y < el.scrollTop || y > el.scrollTop + el.clientHeight - ROW_H) el.scrollTop = y - el.clientHeight / 2;
        }
      });
    });
  }, [sel, dm, expanded]);

  useEffect(() => studio.renameRequest.connect((i) => setRenaming(i.id)), [studio]);
  useEffect(() => studio.panelRequest.connect((req) => {
    const m = req.match(/^explorer:(expand|collapse):(.+)$/);
    if (!m || !dm) return;
    const root = dm.byId(m[2]);
    if (!root) return;
    root.walk((i) => { if (m[1] === 'expand') expanded.add(i.id); else expanded.delete(i.id); });
    setTick((t) => t + 1);
  }), [studio, dm, expanded]);

  useEffect(() => {
    const el = listRef.current;
    if (!el) return;
    const ro = new ResizeObserver(() => setHeight(el.clientHeight));
    ro.observe(el);
    return () => ro.disconnect();
  }, [dm]);

  const rows: Row[] = (() => {
    if (!dm) return [];
    const out: Row[] = [];
    const q = query.trim().toLowerCase();
    if (q) {
      const matches = new Set<Instance>();
      const keep = new Set<Instance>();
      dm.walk((i) => { if (i !== dm && (i.name.toLowerCase().includes(q) || i.className.toLowerCase().includes(q))) matches.add(i); });
      for (const m of matches) { let a: Instance | null = m; while (a && a !== dm) { keep.add(a); a = a.parent; } }
      const visit = (i: Instance, depth: number) => {
        if (!keep.has(i)) return;
        const kids = i.children.filter((c) => keep.has(c));
        out.push({ inst: i, depth, hasKids: kids.length > 0, open: true });
        for (const c of kids) visit(c, depth + 1);
      };
      for (const c of dm.children) visit(c, 0);
      return out;
    }
    const visit = (i: Instance, depth: number) => {
      const open = expanded.has(i.id);
      out.push({ inst: i, depth, hasKids: i.children.length > 0, open });
      if (open) for (const c of i.children) visit(c, depth + 1);
    };
    for (const c of dm.children) visit(c, 0);
    return out;
  })();
  const rowsRef = useRef(rows);
  rowsRef.current = rows;

  if (!view || !dm) return <div className="panel-empty">{t('No project open')}</div>;
  const selection = view.selection;

  const toggle = (i: Instance) => { if (expanded.has(i.id)) expanded.delete(i.id); else expanded.add(i.id); setTick((t) => t + 1); };

  const clickRow = (e: React.MouseEvent, i: Instance) => {
    if (e.shiftKey && anchor.current) {
      const a = rows.findIndex((r) => r.inst === anchor.current), b = rows.findIndex((r) => r.inst === i);
      if (a >= 0 && b >= 0) { const [lo, hi] = a < b ? [a, b] : [b, a]; selection.set(rows.slice(lo, hi + 1).map((r) => r.inst)); return; }
    }
    if (e.ctrlKey || e.metaKey) selection.toggle(i); else selection.set([i]);
    anchor.current = i;
  };

  const commitRename = (i: Instance, value: string) => {
    setRenaming(null);
    const v = value.trim();
    if (v && v !== i.name) view.mutate(t('Rename'), () => i.set('Name', v));
  };

  const onDragStart = (e: React.DragEvent, i: Instance) => {
    if (i.locked) { e.preventDefault(); return; }
    if (!selection.has(i)) selection.set([i]);
    dragged.current = studio.topLevel(selection.list).filter((x) => !x.locked);
    e.dataTransfer.setData('text/forge-instances', dragged.current.map((x) => x.id).join(','));
    e.dataTransfer.effectAllowed = 'move';
  };
  const zoneFor = (e: React.DragEvent, i: Instance): 'before' | 'after' | 'into' => {
    const r = (e.currentTarget as HTMLElement).getBoundingClientRect();
    const f = (e.clientY - r.top) / r.height;
    if (i.locked && i.parent === dm) return 'into';
    return f < 0.25 ? 'before' : f > 0.75 ? 'after' : 'into';
  };
  const onDragOver = (e: React.DragEvent, i: Instance) => {
    if (!dragged.current.length) return;
    const zone = zoneFor(e, i);
    const parent = zone === 'into' ? i : i.parent;
    if (!parent || dragged.current.some((d) => d.canParentTo(parent) || d === i)) { setDropHint(null); return; }
    e.preventDefault();
    e.dataTransfer.dropEffect = 'move';
    if (dropHint?.id !== i.id || dropHint.zone !== zone) setDropHint({ id: i.id, zone });
  };
  const onDrop = (e: React.DragEvent, i: Instance) => {
    e.preventDefault();
    const zone = zoneFor(e, i);
    setDropHint(null);
    const list = dragged.current;
    dragged.current = [];
    if (!list.length) return;
    const parent = zone === 'into' ? i : i.parent!;
    view.mutate(list.length > 1 ? t('Move objects') : t('Move {name}', { name: list[0].name }), () => {
      for (const d of list) {
        if (zone === 'into') { d.setParent(parent); continue; }
        let idx = parent.children.indexOf(i) + (zone === 'after' ? 1 : 0);
        if (d.parent === parent && parent.children.indexOf(d) < idx) idx--;
        d.setParent(parent, idx);
      }
    });
    if (zone === 'into') { expanded.add(i.id); setTick((t) => t + 1); }
  };

  const onKey = (e: React.KeyboardEvent) => {
    if (renaming) return;
    const p = selection.primary;
    const idx = p ? rows.findIndex((r) => r.inst === p) : -1;
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      const n = rows[Math.max(0, Math.min(rows.length - 1, idx + (e.key === 'ArrowDown' ? 1 : -1)))];
      if (n) { selection.set([n.inst]); anchor.current = n.inst; }
    } else if (e.key === 'ArrowRight' && p) { if (!expanded.has(p.id) && p.children.length) toggle(p); }
    else if (e.key === 'ArrowLeft' && p) { if (expanded.has(p.id)) toggle(p); else if (p.parent && p.parent !== dm) selection.set([p.parent]); }
    else if (e.key === 'Enter' && p && p.isA('LuaSourceContainer')) studio.openScript(p);
  };

  const first = Math.max(0, Math.floor(scrollTop / ROW_H) - 5);
  const last = Math.min(rows.length, first + Math.ceil(height / ROW_H) + 12);

  return (
    <div className="explorer">
      <div className="panel-toolbar">
        <div className="search-box">
          <Icon name="search" size={13} />
          <input placeholder={t('Filter workspace (Ctrl+Shift+X)')} value={query} onChange={(e) => setQuery(e.target.value)} data-explorer-search />
          {query && <button className="icon-btn" onClick={() => setQuery('')}><Icon name="close" size={11} /></button>}
        </div>
      </div>
      <div className="tree" ref={listRef} tabIndex={0} onKeyDown={onKey} onScroll={(e) => setScrollTop((e.target as HTMLElement).scrollTop)}
        onMouseDown={(e) => { if (e.target === e.currentTarget) selection.clear(); }}>
        <div style={{ height: rows.length * ROW_H, position: 'relative' }}>
          {rows.slice(first, last).map((r, k) => {
            const i = r.inst;
            const selected = selection.has(i);
            const hint = dropHint?.id === i.id ? ` drop-${dropHint.zone}` : '';
            return (
              <div key={i.id} className={'tree-row' + (selected ? ' selected' : '') + hint + (i.runtimeOnly ? ' runtime' : '')}
                style={{ top: (first + k) * ROW_H, paddingLeft: 4 + r.depth * 14 }}
                draggable={!i.locked && renaming !== i.id}
                onDragStart={(e) => onDragStart(e, i)} onDragOver={(e) => onDragOver(e, i)} onDragLeave={() => setDropHint(null)} onDrop={(e) => onDrop(e, i)}
                onMouseDown={(e) => { if (e.button === 0) clickRow(e, i); }}
                onDoubleClick={() => { if (i.isA('LuaSourceContainer')) studio.openScript(i); else if (!i.locked) setRenaming(i.id); }}
                onContextMenu={(e) => { e.preventDefault(); if (!selection.has(i)) selection.set([i]); studio.showContextMenu(e.clientX, e.clientY, i, 'explorer'); }}>
                <span className="tree-chevron" onMouseDown={(e) => { e.stopPropagation(); if (r.hasKids) toggle(i); }}>
                  {r.hasKids ? <Icon name={r.open ? 'chevronDown' : 'chevronRight'} size={12} /> : null}
                </span>
                <ClassIcon className={i.className} />
                {renaming === i.id ? (
                  <input className="rename-input" autoFocus defaultValue={i.name} onFocus={(e) => e.target.select()}
                    onMouseDown={(e) => e.stopPropagation()}
                    onBlur={(e) => commitRename(i, e.target.value)}
                    onKeyDown={(e) => { e.stopPropagation(); if (e.key === 'Enter') commitRename(i, (e.target as HTMLInputElement).value); if (e.key === 'Escape') setRenaming(null); }} />
                ) : <span className="tree-name">{i.name}</span>}
                {i.isA('BasePart') && i.props.Locked ? <Icon name="lock" size={11} className="dim" /> : null}
                {!view.live && (
                  <button className="row-add" title={t('Insert object')} onMouseDown={(e) => { e.stopPropagation(); const r2 = (e.currentTarget as HTMLElement).getBoundingClientRect(); selection.set([i]); studio.contextMenu.fire({ x: r2.right, y: r2.top, items: insertMenuItems(studio, i) }); }}>
                    <Icon name="plus" size={12} />
                  </button>
                )}
              </div>
            );
          })}
        </div>
      </div>
    </div>
  );
}

// Docking layout: panels live in four zones (left, right-top, right-bottom,
// bottom) as tabs. Panels can be dragged between zones, hidden/shown and the
// zones resized. The layout is persisted in localStorage.

import { useEffect, useRef, useState, type ReactNode } from 'react';
import { Icon } from './icons';

import { t } from '../i18n';
export type ZoneId = 'left' | 'right' | 'right2' | 'bottom';
export interface Layout {
  zones: Record<ZoneId, string[]>;
  active: Partial<Record<ZoneId, string>>;
  hidden: string[];
  size: { left: number; right: number; bottom: number; split: number };
}
export interface PanelDef { id: string; title: string; icon: string; render: () => ReactNode }

export const DEFAULT_LAYOUT: Layout = {
  zones: { left: ['toolbox', 'assets', 'terrain'], right: ['explorer'], right2: ['properties'], bottom: ['output', 'analysis'] },
  active: { left: 'toolbox', right: 'explorer', right2: 'properties', bottom: 'output' },
  hidden: ['terrain', 'analysis'],
  size: { left: 270, right: 300, bottom: 200, split: 0.48 },
};

const KEY = 'forge.layout.v2';
export function loadLayout(): Layout {
  try {
    const raw = JSON.parse(localStorage.getItem(KEY) ?? 'null');
    if (raw && raw.zones && raw.size) {
      const all = new Set(Object.values(DEFAULT_LAYOUT.zones).flat());
      const seen = new Set(Object.values(raw.zones as Record<string, string[]>).flat());
      // add panels introduced after the layout was saved
      for (const p of all) if (!seen.has(p)) { const z = (Object.keys(DEFAULT_LAYOUT.zones) as ZoneId[]).find((k) => DEFAULT_LAYOUT.zones[k].includes(p))!; raw.zones[z].push(p); }
      return raw as Layout;
    }
  } catch { /* corrupt or unavailable storage: use default */ }
  return structuredClone(DEFAULT_LAYOUT);
}
export function saveLayout(l: Layout) { try { localStorage.setItem(KEY, JSON.stringify(l)); } catch { /* ignore */ } }

export function zoneOf(l: Layout, id: string): ZoneId | undefined {
  return (Object.keys(l.zones) as ZoneId[]).find((z) => l.zones[z].includes(id));
}

/** Shows a panel (un-hides it and makes it the active tab of its zone). */
export function showPanel(l: Layout, id: string): Layout {
  const z = zoneOf(l, id) ?? 'left';
  const n: Layout = structuredClone(l);
  if (!n.zones[z].includes(id)) n.zones[z].push(id);
  n.hidden = n.hidden.filter((h) => h !== id);
  n.active[z] = id;
  return n;
}
export function hidePanel(l: Layout, id: string): Layout {
  const n: Layout = structuredClone(l);
  if (!n.hidden.includes(id)) n.hidden.push(id);
  const z = zoneOf(n, id);
  if (z && n.active[z] === id) n.active[z] = n.zones[z].find((p) => !n.hidden.includes(p));
  return n;
}
function movePanel(l: Layout, id: string, to: ZoneId): Layout {
  const n: Layout = structuredClone(l);
  for (const z of Object.keys(n.zones) as ZoneId[]) {
    n.zones[z] = n.zones[z].filter((p) => p !== id);
    if (n.active[z] === id) n.active[z] = n.zones[z].find((p) => !n.hidden.includes(p));
  }
  n.zones[to].push(id);
  n.active[to] = id;
  n.hidden = n.hidden.filter((h) => h !== id);
  return n;
}

function Splitter({ dir, onDrag }: { dir: 'h' | 'v'; onDrag: (delta: number) => void }) {
  const start = (e: React.PointerEvent) => {
    e.preventDefault();
    let last = dir === 'v' ? e.clientX : e.clientY;
    const move = (ev: PointerEvent) => { const cur = dir === 'v' ? ev.clientX : ev.clientY; onDrag(cur - last); last = cur; };
    const up = () => { window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up); document.body.classList.remove('resizing'); };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
    document.body.classList.add('resizing');
  };
  return <div className={`splitter splitter-${dir}`} onPointerDown={start} />;
}

export function DockLayout({ layout, setLayout, panels, center }: { layout: Layout; setLayout: (l: Layout) => void; panels: Record<string, PanelDef>; center: ReactNode }) {
  const [dragging, setDragging] = useState<string | null>(null);
  const rootRef = useRef<HTMLDivElement>(null);
  const [win, setWin] = useState({ w: window.innerWidth, h: window.innerHeight });
  useEffect(() => {
    const r = () => setWin({ w: window.innerWidth, h: window.innerHeight });
    window.addEventListener('resize', r);
    return () => window.removeEventListener('resize', r);
  }, []);
  // keep the center usable on small windows: side zones shrink proportionally
  const eff = {
    left: Math.min(layout.size.left, Math.max(150, win.w * 0.22)),
    right: Math.min(layout.size.right, Math.max(170, win.w * 0.25)),
    bottom: Math.min(layout.size.bottom, Math.max(80, win.h * 0.3)),
  };
  const visible = (z: ZoneId) => layout.zones[z].filter((p) => !layout.hidden.includes(p) && panels[p]);
  const resize = (key: keyof Layout['size'], delta: number, min: number, max: number) => {
    setLayout({ ...layout, size: { ...layout.size, [key]: Math.max(min, Math.min(max, layout.size[key] + delta)) } });
  };

  useEffect(() => {
    const end = () => setDragging(null);
    window.addEventListener('dragend', end);
    return () => window.removeEventListener('dragend', end);
  }, []);

  const zone = (z: ZoneId, style?: React.CSSProperties) => {
    const list = visible(z);
    if (!list.length) return null;
    const active = list.includes(layout.active[z] ?? '') ? layout.active[z]! : list[0];
    return (
      <div className={`dock-zone zone-${z}`} style={style}>
        <div className="dock-tabs">
          {list.map((id) => (
            <div key={id} className={'dock-tab' + (id === active ? ' active' : '')} draggable
              onDragStart={(e) => { e.dataTransfer.setData('text/forge-panel', id); e.dataTransfer.effectAllowed = 'move'; setTimeout(() => setDragging(id)); }}
              onClick={() => setLayout({ ...layout, active: { ...layout.active, [z]: id } })} title={t('Drag to dock elsewhere')}>
              <Icon name={panels[id].icon} size={13} />
              <span>{panels[id].title}</span>
              <button className="tab-close" title={t('Hide panel')} onClick={(e) => { e.stopPropagation(); setLayout(hidePanel(layout, id)); }}><Icon name="close" size={11} /></button>
            </div>
          ))}
        </div>
        {list.map((id) => (
          <div key={id} className="dock-body" style={{ display: id === active ? 'flex' : 'none' }}>{panels[id].render()}</div>
        ))}
      </div>
    );
  };

  const leftVis = visible('left').length > 0;
  const rightVis = visible('right').length > 0 || visible('right2').length > 0;
  const bothRight = visible('right').length > 0 && visible('right2').length > 0;
  const bottomVis = visible('bottom').length > 0;
  const rightHeight = useRef(0);

  const drop = (to: ZoneId) => (e: React.DragEvent) => {
    e.preventDefault();
    const id = e.dataTransfer.getData('text/forge-panel');
    setDragging(null);
    if (id) setLayout(movePanel(layout, id, to));
  };
  const target = (to: ZoneId, label: string) => (
    <div className={`drop-target drop-${to}`} onDragOver={(e) => { e.preventDefault(); e.dataTransfer.dropEffect = 'move'; }} onDrop={drop(to)}>{label}</div>
  );

  return (
    <div className="dock" ref={rootRef}>
      {leftVis && <>{zone('left', { width: eff.left })}<Splitter dir="v" onDrag={(d) => resize('left', d, 160, 600)} /></>}
      <div className="dock-center-col">
        <div className="dock-center">{center}</div>
        {bottomVis && <><Splitter dir="h" onDrag={(d) => resize('bottom', -d, 80, 600)} />{zone('bottom', { height: eff.bottom })}</>}
      </div>
      {rightVis && (
        <>
          <Splitter dir="v" onDrag={(d) => resize('right', -d, 200, 640)} />
          <div className="dock-right-col" style={{ width: eff.right }} ref={(el) => { if (el) rightHeight.current = el.clientHeight; }}>
            {zone('right', bothRight ? { height: `${layout.size.split * 100}%` } : { flex: 1 })}
            {bothRight && <Splitter dir="h" onDrag={(d) => setLayout({ ...layout, size: { ...layout.size, split: Math.max(0.15, Math.min(0.85, layout.size.split + d / Math.max(1, rightHeight.current))) } })} />}
            {zone('right2', { flex: 1 })}
          </div>
        </>
      )}
      {dragging && (
        <div className="drop-overlay">
          {target('left', 'Dock left')}
          {target('right', 'Dock right (top)')}
          {target('right2', 'Dock right (bottom)')}
          {target('bottom', 'Dock bottom')}
        </div>
      )}
    </div>
  );
}

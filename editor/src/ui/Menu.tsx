// Menu bar, dropdown/context menus with submenus and keyboard shortcuts display.

import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import type { ContextMenuItem, Studio } from '../editor/Studio';
import { Icon } from './icons';

export function MenuList({ items, onClose, style }: { items: ContextMenuItem[]; onClose: () => void; style?: React.CSSProperties }) {
  const [open, setOpen] = useState<number | null>(null);
  const ref = useRef<HTMLDivElement>(null);
  const [pos, setPos] = useState<React.CSSProperties>(style ?? {});
  useLayoutEffect(() => {
    // keep menus inside the window
    const el = ref.current;
    if (!el) return;
    const r = el.getBoundingClientRect();
    const fix: React.CSSProperties = {};
    if (r.right > window.innerWidth - 4) fix.left = Math.max(4, window.innerWidth - r.width - 4);
    if (r.bottom > window.innerHeight - 4) fix.top = Math.max(4, window.innerHeight - r.height - 4);
    if (Object.keys(fix).length) setPos({ ...style, ...fix, position: 'fixed' });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  return (
    <div className="menu-list" ref={ref} style={pos} onContextMenu={(e) => e.preventDefault()}>
      {items.map((it, i) => it.separator ? <div key={i} className="menu-sep" /> : (
        <div key={i} className={'menu-item' + (it.disabled ? ' disabled' : '') + (open === i ? ' open' : '')}
          onMouseEnter={() => setOpen(it.children ? i : null)}
          onClick={(e) => {
            e.stopPropagation();
            if (it.disabled || it.children) return;
            onClose();
            it.action?.();
          }}>
          <span className="menu-check">{it.checked ? <Icon name="check" size={12} /> : null}</span>
          <span className="menu-label">{it.label}</span>
          {it.shortcut && <span className="menu-shortcut">{it.shortcut}</span>}
          {it.children && <Icon name="chevronRight" size={12} />}
          {it.children && open === i && !it.disabled && (
            <SubMenu items={it.children} onClose={onClose} />
          )}
        </div>
      ))}
    </div>
  );
}

function SubMenu({ items, onClose }: { items: ContextMenuItem[]; onClose: () => void }) {
  const ref = useRef<HTMLDivElement>(null);
  const [style, setStyle] = useState<React.CSSProperties>({ left: '100%', top: -4 });
  useLayoutEffect(() => {
    const el = ref.current?.firstElementChild as HTMLElement | null;
    if (!el) return;
    const r = el.getBoundingClientRect();
    const s: React.CSSProperties = { left: '100%', top: -4 };
    if (r.right > window.innerWidth - 4) { s.left = 'auto'; s.right = '100%'; }
    if (r.bottom > window.innerHeight - 4) s.top = Math.min(-4, window.innerHeight - r.bottom - 8);
    setStyle(s);
  }, []);
  return <div className="submenu" ref={ref} style={style}><MenuList items={items} onClose={onClose} /></div>;
}

export function MenuBar({ menus }: { menus: { label: string; items: () => ContextMenuItem[] }[] }) {
  const [open, setOpen] = useState<number | null>(null);
  const ref = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (open === null) return;
    const close = (e: MouseEvent) => { if (!ref.current?.contains(e.target as Node)) setOpen(null); };
    const esc = (e: KeyboardEvent) => { if (e.key === 'Escape') setOpen(null); };
    window.addEventListener('mousedown', close);
    window.addEventListener('keydown', esc);
    return () => { window.removeEventListener('mousedown', close); window.removeEventListener('keydown', esc); };
  }, [open]);
  return (
    <div className="menubar" ref={ref}>
      <div className="app-logo" title="WorldRidr Studio"><svg width="18" height="18" viewBox="0 0 16 16"><path d="M8 1 14.5 4.8v6.4L8 15 1.5 11.2V4.8z" fill="#2f7cf6" /><path d="M5.2 5h5.6v1.6H7v1.2h3.2v1.6H7V12H5.2z" fill="#fff" /></svg></div>
      {menus.map((m, i) => (
        <div key={m.label} className={'menubar-item' + (open === i ? ' open' : '')}
          onMouseDown={() => setOpen(open === i ? null : i)}
          onMouseEnter={() => { if (open !== null) setOpen(i); }}>
          {m.label}
          {open === i && <div className="menubar-dropdown"><MenuList items={m.items()} onClose={() => setOpen(null)} /></div>}
        </div>
      ))}
    </div>
  );
}

export function ContextMenuHost({ studio }: { studio: Studio }) {
  const [menu, setMenu] = useState<{ x: number; y: number; items: ContextMenuItem[] } | null>(null);
  useEffect(() => studio.contextMenu.connect((m) => setMenu(m)), [studio]);
  useEffect(() => {
    if (!menu) return;
    const close = () => setMenu(null);
    const t = setTimeout(() => { window.addEventListener('mousedown', close); window.addEventListener('blur', close); });
    const esc = (e: KeyboardEvent) => { if (e.key === 'Escape') close(); };
    window.addEventListener('keydown', esc);
    return () => { clearTimeout(t); window.removeEventListener('mousedown', close); window.removeEventListener('blur', close); window.removeEventListener('keydown', esc); };
  }, [menu]);
  if (!menu) return null;
  return (
    <div className="context-menu" onMouseDown={(e) => e.stopPropagation()}>
      <MenuList items={menu.items} onClose={() => setMenu(null)} style={{ position: 'fixed', left: menu.x, top: menu.y }} />
    </div>
  );
}

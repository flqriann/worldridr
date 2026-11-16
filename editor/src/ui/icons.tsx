// Original icon set (simple geometric SVG glyphs). Class icons use a colored
// glyph so the Explorer is easy to scan.

import type { CSSProperties } from 'react';
import { getClass } from '../core/classes';

type IconDef = { d: string; color?: string; fill?: boolean };

const I: Record<string, IconDef> = {
  // classes
  instance: { d: 'M3 3h10v10H3z', color: '#9aa4b2' },
  game: { d: 'M8 1.5 14 5v6l-6 3.5L2 11V5zM8 1.5V8m0 0 6-3M8 8 2 5m6 3v6.5', color: '#9aa4b2' },
  workspace: { d: 'M8 1.5a6.5 6.5 0 1 0 0 13 6.5 6.5 0 0 0 0-13zM1.5 8h13M8 1.5c2 2 2 11 0 13M8 1.5c-2 2-2 11 0 13', color: '#4fb3ff' },
  lighting: { d: 'M8 2v1.5M8 12.5V14M2 8h1.5M12.5 8H14M3.8 3.8l1 1M11.2 11.2l1 1M3.8 12.2l1-1M11.2 4.8l1-1M8 5a3 3 0 1 0 0 6 3 3 0 0 0 0-6z', color: '#ffd24d' },
  storage: { d: 'M2.5 4.5h11v8h-11zM2.5 4.5l1.5-2h8l1.5 2M6 7.5h4', color: '#c792ea' },
  scriptservice: { d: 'M2.5 2.5h11v11h-11zM5 6l2 2-2 2M8.5 10.5H11', color: '#7fd88f' },
  startergui: { d: 'M2 3h12v9H2zM5 14.5h6M8 12v2.5M4.5 5.5h4', color: '#ff9f6b' },
  starterplayer: { d: 'M8 2.5a2.2 2.2 0 1 0 0 4.4 2.2 2.2 0 0 0 0-4.4zM3.5 14c.4-3 2.2-5 4.5-5s4.1 2 4.5 5', color: '#6bd5ff' },
  players: { d: 'M6 3a2 2 0 1 0 0 4 2 2 0 0 0 0-4zM2.5 13c.3-2.7 1.7-4.5 3.5-4.5S9.2 10.3 9.5 13M11 4a1.7 1.7 0 1 1 0 3.4M11.5 9c1.3.3 2.1 1.7 2.3 4', color: '#6bd5ff' },
  player: { d: 'M8 2.5a2.2 2.2 0 1 0 0 4.4 2.2 2.2 0 0 0 0-4.4zM3.5 14c.4-3 2.2-5 4.5-5s4.1 2 4.5 5', color: '#6bd5ff' },
  humanoid: { d: 'M8 1.8a1.6 1.6 0 1 0 0 3.2 1.6 1.6 0 0 0 0-3.2zM5 6.5h6M8 6.5v4M8 10.5 5.5 14M8 10.5l2.5 3.5', color: '#ff8fa3' },
  remote: { d: 'M2 5.5h9.5M9 3l2.5 2.5L9 8M14 10.5H4.5M7 8l-2.5 2.5L7 13', color: '#ffd166' },
  sound: { d: 'M2.5 6h3l3.5-3v10L5.5 10h-3zM11 5.5c1 1.4 1 3.6 0 5M12.8 4c1.8 2.4 1.8 5.6 0 8', color: '#ffb86b' },
  terrain: { d: 'M1.5 13 5.5 6l2.5 3.5L10.5 5l4 8z', color: '#6fcf6f' },
  sky: { d: 'M4.5 11.5a3 3 0 0 1 .3-6 4 4 0 0 1 7.4 1.2 2.4 2.4 0 0 1-.2 4.8z', color: '#8ec8ff' },
  atmosphere: { d: 'M2 6h9a2 2 0 1 0-2-2M2 9h12M2 12h8a2 2 0 1 1-2 2', color: '#a8c6e8' },
  part: { d: 'M8 1.8 14 5v6.2L8 14.4 2 11.2V5zM2 5l6 3.2L14 5M8 8.2v6.2', color: '#8fb4ff' },
  wedge: { d: 'M2 12.5h12V4.5zM2 12.5 6 15M14 4.5l-1 6', color: '#8fb4ff' },
  spawn: { d: 'M3 11.5h10v2H3zM8 2v7.5M5 5l3-3 3 3', color: '#5ad18a' },
  mesh: { d: 'M8 1.5 14 5v6L8 14.5 2 11V5zM2 5l12 6M14 5 2 11M8 1.5v13', color: '#d48fff' },
  model: { d: 'M2 4.5 8 1.5l6 3v7l-6 3-6-3zM2 4.5l6 3 6-3M8 7.5v7', color: '#ffcf6b' },
  folder: { d: 'M1.5 4h4.5l1.5 1.5h7v7.5h-13z', color: '#e6c06e' },
  script: { d: 'M3.5 1.5h6l3 3v10h-9zM9.5 1.5v3h3M5.5 8h5M5.5 10.5h5M5.5 5.5h2', color: '#7fd88f' },
  localscript: { d: 'M3.5 1.5h6l3 3v10h-9zM9.5 1.5v3h3M5.5 8h5M5.5 10.5h3', color: '#6bc7ff' },
  modulescript: { d: 'M3.5 1.5h6l3 3v10h-9zM9.5 1.5v3h3M6 7.5h4v4H6z', color: '#c792ea' },
  light: { d: 'M8 1.5a4 4 0 0 0-2.5 7.2V11h5V8.7A4 4 0 0 0 8 1.5zM6 13h4M6.8 14.8h2.4', color: '#ffe066' },
  pointlight: { d: 'M8 1.5a4 4 0 0 0-2.5 7.2V11h5V8.7A4 4 0 0 0 8 1.5zM6 13h4M6.8 14.8h2.4', color: '#ffe066' },
  spotlight: { d: 'M5.5 2h5l-1 4h-3zM6.5 6 2.5 14h11l-4-8', color: '#ffe066' },
  particles: { d: 'M4 12a1.2 1.2 0 1 0 0 .1M8 8.5a1.5 1.5 0 1 0 0 .1M12 4a1.8 1.8 0 1 0 0 .1M11 11.5a1 1 0 1 0 0 .1M5 4.5a.8.8 0 1 0 0 .1', color: '#ff7ad9' },
  animation: { d: 'M2.5 3.5h11v9h-11zM2.5 6h11M5.5 3.5 4 6M9 3.5 7.5 6M12.5 3.5 11 6M7 8l3 1.8L7 11.5z', color: '#ffa94d' },
  click: { d: 'M5 2v8.5l2.2-2 1.6 3.8 1.6-.7-1.6-3.6 3-.2z', color: '#e0e0e0' },
  camera: { d: 'M1.5 5h9v7h-9zM10.5 7.2l4-2v6.6l-4-2', color: '#9fb8d6' },
  screengui: { d: 'M2 3h12v9H2zM5 14.5h6M8 12v2.5', color: '#ff9f6b' },
  frame: { d: 'M2.5 3.5h11v9h-11z', color: '#ff9f6b' },
  textlabel: { d: 'M3 4h10M8 4v9M5.5 13h5', color: '#ff9f6b' },
  textbutton: { d: 'M2 4.5h12v7H2zM5 8h6', color: '#ff9f6b' },
  imagelabel: { d: 'M2 3h12v10H2zM2 11l4-4 3 3 2-2 3 3M10.5 5.5a.8.8 0 1 0 0 .1', color: '#ff9f6b' },
  value: { d: 'M3 3.5h10v9H3zM5.5 6.5h5M5.5 9.5h3', color: '#80cbc4' },

  // UI glyphs
  chevronRight: { d: 'M6 4l4 4-4 4' },
  chevronDown: { d: 'M4 6l4 4 4-4' },
  close: { d: 'M4 4l8 8M12 4l-8 8' },
  plus: { d: 'M8 3v10M3 8h10' },
  search: { d: 'M7 2.5a4.5 4.5 0 1 0 0 9 4.5 4.5 0 0 0 0-9zM10.3 10.3 14 14' },
  select: { d: 'M3.5 2v11l3-2.6 2 4 1.8-.8-2-3.9H12z', fill: true },
  move: { d: 'M8 1.5v13M1.5 8h13M8 1.5 6 3.5M8 1.5l2 2M8 14.5l-2-2M8 14.5l2-2M1.5 8l2-2M1.5 8l2 2M14.5 8l-2-2M14.5 8l-2 2' },
  scale: { d: 'M2 14 14 2M9 2h5v5M7 14H2V9' },
  rotate: { d: 'M13 8a5 5 0 1 1-1.5-3.6M13 2.5v3h-3' },
  play: { d: 'M4.5 2.5v11l9-5.5z', fill: true },
  stop: { d: 'M3.5 3.5h9v9h-9z', fill: true },
  pause: { d: 'M4 3h3v10H4zM9 3h3v10H9z', fill: true },
  restart: { d: 'M3 8a5 5 0 1 0 1.5-3.6M3 2.5v3h3' },
  undo: { d: 'M5.5 3 2.5 6l3 3M2.5 6H10a3.5 3.5 0 0 1 0 7H6' },
  redo: { d: 'M10.5 3l3 3-3 3M13.5 6H6a3.5 3.5 0 0 0 0 7h4' },
  copy: { d: 'M5.5 5.5h8v8h-8zM2.5 10.5v-8h8' },
  cut: { d: 'M4.5 11a2 2 0 1 0 0 .1M11.5 11a2 2 0 1 0 0 .1M5.8 9.6 12 2M10.2 9.6 4 2' },
  paste: { d: 'M5 2.5h6v2H5zM3.5 3.5h-1v11h11v-11h-1' },
  duplicate: { d: 'M2.5 2.5h8v8h-8zM5.5 13.5h8v-8M6.5 4.5v4M4.5 6.5h4' },
  trash: { d: 'M3 4.5h10M6.5 4.5v-2h3v2M4.5 4.5l.7 9.5h5.6l.7-9.5' },
  group: { d: 'M1.5 1.5h13v13h-13zM4 4h4v4H4zM8 8h4v4H8z' },
  ungroup: { d: 'M2 2h5v5H2zM9 9h5v5H9z' },
  anchor: { d: 'M8 4.5a1.5 1.5 0 1 0 0-3 1.5 1.5 0 0 0 0 3zM8 4.5v10M5 7.5h6M2.5 10c0 2.5 2.5 4.5 5.5 4.5s5.5-2 5.5-4.5' },
  lock: { d: 'M4 7.5h8v6.5H4zM5.5 7.5V5a2.5 2.5 0 0 1 5 0v2.5' },
  collide: { d: 'M2 6h6v6H2zM8 4h6v6H8z' },
  color: { d: 'M8 2a6 6 0 1 0 0 12c1 0 1.5-.6 1.5-1.3 0-1.2-1-1.3-1-2.2 0-.8.6-1 1.5-1H12a2.5 2.5 0 0 0 2-2.5C14 4.3 11.3 2 8 2zM4.5 8a.8.8 0 1 0 0 .1M6 5a.8.8 0 1 0 0 .1M10 5a.8.8 0 1 0 0 .1' },
  material: { d: 'M2 2h12v12H2zM2 6h12M2 10h12M6 2v12M10 2v12' },
  grid: { d: 'M2 2h12v12H2zM6 2v12M10 2v12M2 6h12M2 10h12' },
  stats: { d: 'M2.5 13.5V9M6 13.5V5M9.5 13.5V7M13 13.5v-11' },
  save: { d: 'M2.5 2.5h9l2 2v9h-11zM5 2.5v4h6v-4M5 13.5V9h6v4.5' },
  open: { d: 'M1.5 4h4.5l1.5 1.5h7v7.5h-13zM1.5 7.5h13' },
  new: { d: 'M3.5 1.5h6l3 3v10h-9zM9.5 1.5v3h3M8 7v5M5.5 9.5h5' },
  importIcon: { d: 'M8 2v8M5 7l3 3 3-3M2.5 11v2.5h11V11' },
  exportIcon: { d: 'M8 10V2M5 5l3-3 3 3M2.5 11v2.5h11V11' },
  brush: { d: 'M13.5 2.5 7 9l-1.5-1.5L12 1zM5.5 9.5c-2 0-3 1.5-3 4 2.5 0 4-1 4-3z' },
  mountain: { d: 'M1.5 13 6 5l3 5 2-3 3.5 6z' },
  building: { d: 'M3 14V4.5L8 2l5 2.5V14zM6 6h1.5M9 6h1.5M6 9h1.5M9 9h1.5M7 14v-2.5h2V14', color: '#e0b878' },
  road: { d: 'M5 14 6.5 2M11 14 9.5 2M8 3v2M8 7v2M8 11v2', color: '#9aa4b2' },
  water: { d: 'M8 2s-4.5 5-4.5 8a4.5 4.5 0 0 0 9 0C12.5 7 8 2 8 2z' },
  eye: { d: 'M1.5 8S4 3.5 8 3.5 14.5 8 14.5 8 12 12.5 8 12.5 1.5 8 1.5 8zM8 6a2 2 0 1 0 0 4 2 2 0 0 0 0-4z' },
  layout: { d: 'M2 2h12v12H2zM2 5h12M6 5v9' },
  help: { d: 'M8 1.5a6.5 6.5 0 1 0 0 13 6.5 6.5 0 0 0 0-13zM6 6a2 2 0 1 1 2.8 1.8c-.5.3-.8.7-.8 1.3v.4M8 11.5v.2' },
  format: { d: 'M2 3h12M5 6.5h9M5 10h9M2 13.5h12' },
  bug: { d: 'M5 5.5h6v6a3 3 0 0 1-6 0zM6 5.5a2 2 0 0 1 4 0M2.5 8H5M11 8h2.5M3 12l2-1M13 12l-2-1M3 4l2 2M13 4l-2 2' },
  world: { d: 'M8 1.5a6.5 6.5 0 1 0 0 13 6.5 6.5 0 0 0 0-13zM1.5 8h13M8 1.5c2 2 2 11 0 13M8 1.5c-2 2-2 11 0 13' },
  local: { d: 'M8 2 14 5v6l-6 3-6-3V5zM8 8v6M8 8l6-3M8 8 2 5' },
  check: { d: 'M3 8.5 6.5 12 13 4.5' },
  warn: { d: 'M8 2 14.5 13.5h-13zM8 6.5v3.5M8 11.8v.2' },
  error: { d: 'M8 1.5a6.5 6.5 0 1 0 0 13 6.5 6.5 0 0 0 0-13zM5.5 5.5l5 5M10.5 5.5l-5 5' },
  info: { d: 'M8 1.5a6.5 6.5 0 1 0 0 13 6.5 6.5 0 0 0 0-13zM8 7v4.5M8 4.6v.2' },
  dots: { d: 'M3.5 8h.1M8 8h.1M12.5 8h.1' },
  ball: { d: 'M8 1.5a6.5 6.5 0 1 0 0 13 6.5 6.5 0 0 0 0-13zM3 5.5c3 1.5 7 1.5 10 0', color: '#8fb4ff' },
  cylinder: { d: 'M3 4a5 2 0 1 0 10 0 5 2 0 1 0-10 0M3 4v8a5 2 0 0 0 10 0V4', color: '#8fb4ff' },
  focus: { d: 'M2 5V2h3M11 2h3v3M14 11v3h-3M5 14H2v-3M8 6a2 2 0 1 0 0 4 2 2 0 0 0 0-4z' },
  library: { d: 'M2.5 2.5h3v11h-3zM6.5 2.5h3v11h-3zM10.5 3l2.8-.8 2.2 10.5-2.8.8z' },
  terminal: { d: 'M2 3h12v10H2zM4.5 6l2 2-2 2M8 10.5h3.5' },
};

export function Icon({ name, size = 16, color, style, className }: { name: string; size?: number; color?: string; style?: CSSProperties; className?: string }) {
  const def = I[name] ?? I.instance;
  const c = color ?? def.color ?? 'currentColor';
  return (
    <svg className={'icon ' + (className ?? '')} width={size} height={size} viewBox="0 0 16 16" style={style} aria-hidden>
      <path d={def.d} fill={def.fill ? c : 'none'} stroke={def.fill ? 'none' : c} strokeWidth={1.4} strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  );
}

export function ClassIcon({ className: cls, size = 16 }: { className: string; size?: number }) {
  return <Icon name={iconForClass(cls)} size={size} />;
}

export function iconForClass(cls: string): string {
  if (cls === 'Part') return 'part';
  return getClass(cls)?.icon ?? 'instance';
}

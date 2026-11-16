// Renders ScreenGui hierarchies (Frame/TextLabel/TextButton/ImageLabel) as
// absolutely positioned DOM elements over the viewport, including visual
// effects (background blur, gradients, strokes, shadows). In UI-editor mode
// elements can be selected, dragged and resized directly on screen.

import type { DataModel, Instance } from '../core/Instance';
import { udim2, type UDim2 } from '../core/types';
import type { AssetStore } from '../assets/AssetStore';
import { assetDataUrl } from '../assets/AssetStore';
import type { Document } from '../editor/Document';

const FONTS: Record<string, string> = {
  Sans: '500 {s}px "Segoe UI", system-ui, sans-serif',
  SansBold: '700 {s}px "Segoe UI", system-ui, sans-serif',
  Serif: '500 {s}px Georgia, serif',
  Mono: '500 {s}px Consolas, "Cascadia Code", monospace',
  Cartoon: '700 {s}px "Comic Sans MS", "Segoe Print", cursive',
};

function rgba(hex: string, transparency: number) {
  const n = parseInt(hex.slice(1), 16);
  return `rgba(${(n >> 16) & 255},${(n >> 8) & 255},${n & 255},${Math.max(0, Math.min(1, 1 - transparency))})`;
}

type HandleDir = 'n' | 's' | 'e' | 'w' | 'ne' | 'nw' | 'se' | 'sw';
const HANDLES: HandleDir[] = ['nw', 'n', 'ne', 'e', 'se', 's', 'sw', 'w'];
const SNAP = 6; // px distance for center guides

export class GuiRenderer {
  readonly root: HTMLDivElement;
  private els = new Map<Instance, HTMLElement>();
  private byEl = new WeakMap<HTMLElement, Instance>();
  private container: Instance | null = null;
  private unsub: (() => void)[] = [];
  private interactive = false;
  private editDoc: Document | null = null;
  private editUnsub: (() => void)[] = [];
  private overlay: HTMLDivElement;
  private guideV: HTMLDivElement;
  private guideH: HTMLDivElement;
  onButtonClick: (inst: Instance) => void = () => {};
  onMouseEnter: (inst: Instance, enter: boolean) => void = () => {};

  constructor(parent: HTMLElement, private assets: () => AssetStore | null) {
    this.root = document.createElement('div');
    this.root.className = 'gui-root';
    parent.appendChild(this.root);
    this.overlay = document.createElement('div');
    this.overlay.className = 'gui-edit-overlay';
    this.guideV = document.createElement('div'); this.guideV.className = 'gui-guide v';
    this.guideH = document.createElement('div'); this.guideH.className = 'gui-guide h';
    parent.appendChild(this.overlay);
    this.overlay.append(this.guideV, this.guideH);
  }

  /** Shows the ScreenGuis under `container` (StarterGui in edit mode, PlayerGui in play mode). */
  setContainer(dm: DataModel | null, container: Instance | null, interactive: boolean) {
    this.unsub.forEach((u) => u());
    this.unsub = [];
    this.container = container;
    this.interactive = interactive;
    if (interactive) this.setEditing(null);
    this.rebuild();
    if (!dm || !container) return;
    const relevant = (i: Instance) => i === container || i.isDescendantOf(container);
    let pending = false;
    const schedule = () => { if (!pending) { pending = true; queueMicrotask(() => { pending = false; this.rebuild(); }); } };
    this.unsub.push(
      dm.added.connect((i) => { if (relevant(i)) schedule(); }),
      dm.removed.connect((i) => { if (this.els.has(i)) schedule(); }),
      dm.reparented.connect((i) => { if (relevant(i) || this.els.has(i)) schedule(); }),
      dm.changed.connect((i) => {
        const el = this.els.get(i);
        if (el) { this.style(i, el); this.updateHandles(); } else if (i.className === 'ScreenGui' && relevant(i)) schedule();
      }),
    );
  }

  // ---- rendering -------------------------------------------------------------------

  private rebuild() {
    this.root.replaceChildren();
    this.els.clear();
    if (this.container) {
      const guis = this.container.children.filter((c) => c.className === 'ScreenGui').sort((a, b) => (a.props.DisplayOrder as number) - (b.props.DisplayOrder as number));
      for (const g of guis) {
        const layer = document.createElement('div');
        layer.className = 'gui-layer';
        layer.style.display = g.props.Enabled ? 'block' : 'none';
        this.els.set(g, layer);
        this.root.appendChild(layer);
        for (const c of g.children) this.build(c, layer);
      }
    }
    this.updateHandles();
  }

  private build(inst: Instance, parentEl: HTMLElement) {
    if (!inst.isA('GuiObject')) return;
    const el = document.createElement(inst.className === 'TextButton' ? 'button' : 'div');
    el.className = 'gui-obj';
    if (inst.className === 'TextButton') {
      el.addEventListener('click', (e) => { e.stopPropagation(); if (this.interactive) this.onButtonClick(inst); });
    }
    el.addEventListener('mouseenter', () => { if (this.interactive) this.onMouseEnter(inst, true); });
    el.addEventListener('mouseleave', () => { if (this.interactive) this.onMouseEnter(inst, false); });
    el.addEventListener('pointerdown', (e) => this.onEditPointerDown(e as PointerEvent, inst));
    this.els.set(inst, el);
    this.byEl.set(el, inst);
    this.style(inst, el);
    parentEl.appendChild(el);
    for (const c of inst.children) this.build(c, el);
  }

  private style(inst: Instance, el: HTMLElement) {
    const p = inst.props;
    if (inst.className === 'ScreenGui') { el.style.display = p.Enabled ? 'block' : 'none'; return; }
    const pos = p.Position as UDim2, size = p.Size as UDim2;
    const s = el.style;
    s.left = `calc(${pos.xs * 100}% + ${pos.xo}px)`;
    s.top = `calc(${pos.ys * 100}% + ${pos.yo}px)`;
    s.width = `calc(${size.xs * 100}% + ${size.xo}px)`;
    s.height = `calc(${size.ys * 100}% + ${size.yo}px)`;
    s.transform = `translate(${-(p.AnchorPointX as number) * 100}%, ${-(p.AnchorPointY as number) * 100}%)`;
    const bgT = p.BackgroundTransparency as number;
    const bg = rgba(p.BackgroundColor3 as string, bgT);
    s.background = p.GradientEnabled
      ? `linear-gradient(${p.GradientRotation}deg, ${bg}, ${rgba(p.GradientColor as string, bgT)})`
      : bg;
    const bw = p.BorderSizePixel as number;
    s.border = bw > 0 ? `${bw}px solid ${p.BorderColor3}` : 'none';
    s.borderRadius = `${p.CornerRadius}px`;
    const blur = p.BackgroundBlur as number;
    s.backdropFilter = blur > 0 ? `blur(${blur}px) saturate(1.2)` : '';
    (s as CSSStyleDeclaration & { webkitBackdropFilter: string }).webkitBackdropFilter = s.backdropFilter;
    const st = p.StrokeThickness as number;
    s.outline = st > 0 ? `${st}px solid ${rgba(p.StrokeColor as string, p.StrokeTransparency as number)}` : '';
    s.outlineOffset = st > 0 ? `${-st}px` : '';
    const sh = p.ShadowSize as number;
    s.boxShadow = sh > 0 ? `0 ${Math.round(sh * 0.35)}px ${sh}px ${rgba('#000000', p.ShadowTransparency as number)}` : '';
    s.display = p.Visible ? 'flex' : 'none';
    s.zIndex = String(p.ZIndex);
    s.pointerEvents = this.editDoc ? 'auto' : this.interactive && inst.className === 'TextButton' ? 'auto' : 'none';
    if ('Text' in p) {
      // keep child GUI elements after replacing the text node
      const kids = inst.children.map((c) => this.els.get(c)).filter((x): x is HTMLElement => !!x);
      el.textContent = p.Text as string;
      for (const k of kids) el.appendChild(k);
      s.color = p.TextColor3 as string;
      const px = p.TextScaled ? Math.max(8, Math.min(200, ((el.clientHeight || 40) * 0.7))) : (p.TextSize as number);
      s.font = (FONTS[p.Font as string] ?? FONTS.Sans).replace('{s}', String(px));
      s.justifyContent = p.TextXAlignment === 'Left' ? 'flex-start' : p.TextXAlignment === 'Right' ? 'flex-end' : 'center';
    }
    if (inst.className === 'ImageLabel') {
      const a = this.assets()?.get(p.Image as string);
      s.backgroundImage = a && a.kind === 'texture' ? `url("${assetDataUrl(a)}")` : 'none';
      s.backgroundSize = '100% 100%';
      s.opacity = String(1 - (p.ImageTransparency as number));
    }
    el.classList.toggle('gui-selected', !!this.editDoc?.selection.has(inst));
  }

  // ---- UI editor -------------------------------------------------------------------

  /** Enables on-screen editing for the StarterGui of `doc` (null = off). */
  setEditing(doc: Document | null) {
    this.editUnsub.forEach((u) => u());
    this.editUnsub = [];
    this.editDoc = doc && !this.interactive ? doc : null;
    this.root.classList.toggle('editing', !!this.editDoc);
    this.overlay.style.display = this.editDoc ? 'block' : 'none';
    if (this.editDoc) {
      const d = this.editDoc;
      this.editUnsub.push(d.selection.changed.connect(() => { for (const [i, el] of this.els) this.style(i, el); this.updateHandles(); }));
      const key = (e: KeyboardEvent) => this.onEditKey(e);
      window.addEventListener('keydown', key);
      this.editUnsub.push(() => window.removeEventListener('keydown', key));
    }
    for (const [i, el] of this.els) this.style(i, el);
    this.updateHandles();
  }

  private selectedGui(): Instance[] {
    return this.editDoc ? this.editDoc.selection.list.filter((i) => i.isA('GuiObject') && this.els.has(i)) : [];
  }

  private parentSize(inst: Instance): { w: number; h: number } {
    const el = this.els.get(inst);
    const parent = el?.parentElement;
    return { w: parent?.clientWidth || this.root.clientWidth || 1, h: parent?.clientHeight || this.root.clientHeight || 1 };
  }

  private onEditPointerDown(e: PointerEvent, inst: Instance) {
    const doc = this.editDoc;
    if (!doc || e.button !== 0) return;
    e.stopPropagation();
    e.preventDefault();
    if (e.shiftKey || e.ctrlKey) doc.selection.toggle(inst);
    else if (!doc.selection.has(inst)) doc.selection.set([inst]);
    const items = this.selectedGui();
    if (!items.includes(inst)) return;
    this.startDrag(e, items, (it, dx, dy, st) => {
      const pos = st.pos;
      return { pos: udim2(pos.xs, Math.round(pos.xo + dx), pos.ys, Math.round(pos.yo + dy)), size: st.size };
    }, true);
  }

  /** Generic drag: `apply` maps the pointer delta to a new Position/Size per item. */
  private startDrag(e: PointerEvent, items: Instance[], apply: (inst: Instance, dx: number, dy: number, start: { pos: UDim2; size: UDim2 }) => { pos: UDim2; size: UDim2 }, guides: boolean) {
    const doc = this.editDoc!;
    const starts = new Map(items.map((i) => [i, { pos: i.props.Position as UDim2, size: i.props.Size as UDim2 }]));
    const x0 = e.clientX, y0 = e.clientY;
    let moved = false;
    const move = (ev: PointerEvent) => {
      let dx = ev.clientX - x0, dy = ev.clientY - y0;
      if (!moved && Math.abs(dx) + Math.abs(dy) < 3) return;
      moved = true;
      if (!ev.altKey) { dx = Math.round(dx / 2) * 2; dy = Math.round(dy / 2) * 2; }
      this.guideV.style.display = 'none';
      this.guideH.style.display = 'none';
      if (guides && items.length === 1) {
        // snap the element's center to its parent's center lines
        const it = items[0];
        const el = this.els.get(it)!;
        const parent = el.parentElement!;
        const pr = parent.getBoundingClientRect();
        const st = starts.get(it)!;
        const trial = apply(it, dx, dy, st);
        const { w, h } = this.parentSize(it);
        const ax = it.props.AnchorPointX as number, ay = it.props.AnchorPointY as number;
        const ew = trial.size.xs * w + trial.size.xo, eh = trial.size.ys * h + trial.size.yo;
        const left = trial.pos.xs * w + trial.pos.xo - ax * ew, top = trial.pos.ys * h + trial.pos.yo - ay * eh;
        const centerX = left + ew / 2, centerY = top + eh / 2;
        if (Math.abs(centerX - w / 2) < SNAP) { dx += w / 2 - centerX; this.guideV.style.display = 'block'; this.guideV.style.left = `${pr.left - this.overlay.getBoundingClientRect().left + w / 2}px`; this.guideV.style.top = `${pr.top - this.overlay.getBoundingClientRect().top}px`; this.guideV.style.height = `${h}px`; }
        if (Math.abs(centerY - h / 2) < SNAP) { dy += h / 2 - centerY; this.guideH.style.display = 'block'; this.guideH.style.top = `${pr.top - this.overlay.getBoundingClientRect().top + h / 2}px`; this.guideH.style.left = `${pr.left - this.overlay.getBoundingClientRect().left}px`; this.guideH.style.width = `${w}px`; }
      }
      for (const it of items) {
        const r = apply(it, dx, dy, starts.get(it)!);
        it.setSilent('Position', r.pos);
        it.setSilent('Size', r.size);
      }
    };
    const up = () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
      this.guideV.style.display = 'none';
      this.guideH.style.display = 'none';
      if (!moved) return;
      const finals = items.map((i) => ({ i, pos: i.props.Position as UDim2, size: i.props.Size as UDim2 }));
      for (const f of finals) { const st = starts.get(f.i)!; f.i.setSilent('Position', st.pos); f.i.setSilent('Size', st.size); }
      doc.transact(items.length > 1 ? 'Move UI' : `Edit ${items[0].name}`, () => { for (const f of finals) { f.i.set('Position', f.pos); f.i.set('Size', f.size); } });
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
  }

  private updateHandles() {
    for (const h of [...this.overlay.querySelectorAll('.gui-handle')]) h.remove();
    const items = this.selectedGui();
    if (!this.editDoc || items.length !== 1) return;
    const inst = items[0];
    const el = this.els.get(inst)!;
    const r = el.getBoundingClientRect(), o = this.overlay.getBoundingClientRect();
    if (!r.width && !r.height) return;
    const pts: Record<HandleDir, [number, number]> = {
      nw: [r.left, r.top], n: [r.left + r.width / 2, r.top], ne: [r.right, r.top], e: [r.right, r.top + r.height / 2],
      se: [r.right, r.bottom], s: [r.left + r.width / 2, r.bottom], sw: [r.left, r.bottom], w: [r.left, r.top + r.height / 2],
    };
    for (const dir of HANDLES) {
      const h = document.createElement('div');
      h.className = `gui-handle h-${dir}`;
      h.style.left = `${pts[dir][0] - o.left}px`;
      h.style.top = `${pts[dir][1] - o.top}px`;
      h.addEventListener('pointerdown', (e) => {
        e.stopPropagation(); e.preventDefault();
        const ax = inst.props.AnchorPointX as number, ay = inst.props.AnchorPointY as number;
        this.startDrag(e, [inst], (_i, dx, dy, st) => {
          let { xs, xo, ys, yo } = st.pos;
          let sw = st.size.xo, sh = st.size.yo;
          if (dir.includes('e')) { sw += dx; xo += ax * dx; }
          if (dir.includes('w')) { sw -= dx; xo += (1 - ax) * dx; }
          if (dir.includes('s')) { sh += dy; yo += ay * dy; }
          if (dir.includes('n')) { sh -= dy; yo += (1 - ay) * dy; }
          return { pos: udim2(xs, Math.round(xo), ys, Math.round(yo)), size: udim2(st.size.xs, Math.round(sw), st.size.ys, Math.round(sh)) };
        }, false);
      });
      this.overlay.appendChild(h);
    }
  }

  private onEditKey(e: KeyboardEvent) {
    const items = this.selectedGui();
    if (!items.length || !this.editDoc) return;
    const el = e.target as HTMLElement | null;
    if (el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.closest?.('.monaco-editor'))) return;
    const step = e.shiftKey ? 10 : 1;
    const d = { ArrowLeft: [-step, 0], ArrowRight: [step, 0], ArrowUp: [0, -step], ArrowDown: [0, step] }[e.key as 'ArrowLeft'];
    if (!d) return;
    e.preventDefault();
    e.stopPropagation();
    this.editDoc.transact('Nudge UI', () => {
      for (const i of items) { const p = i.props.Position as UDim2; i.set('Position', udim2(p.xs, p.xo + d[0], p.ys, p.yo + d[1])); }
    }, 'nudge-ui');
  }

  /** Re-positions the resize handles (e.g. after the viewport was resized). */
  refreshHandles() { this.updateHandles(); }

  dispose() { this.unsub.forEach((u) => u()); this.editUnsub.forEach((u) => u()); this.root.remove(); this.overlay.remove(); }
}

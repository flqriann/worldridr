// Ribbon toolbar (Home / Model / Test / View / Terrain tabs).

import { useEffect, useState } from 'react';
import type { Studio } from '../editor/Studio';
import type { Tool } from '../render/Viewport';
import { MATERIALS } from '../core/classes';
import { materialPreviewUrl } from '../render/materials';
import { Icon } from './icons';
import { useSignals } from './hooks';
import type { Layout } from './Dock';

import { t } from '../i18n';
const PALETTE = ['#f2f3f3', '#a3a2a5', '#635f62', '#1b2a35', '#c4281c', '#da8541', '#f5cd30', '#a4bd47', '#4b974b', '#0d69ac', '#6e99ca', '#b4d2e4', '#8b4513', '#d7c59a', '#ff66cc', '#9a5ccf', '#ffcc33', '#2ee6a6', '#00a2ff', '#111111'];

type BtnProps = { icon: string; label: string; onClick: (e: React.MouseEvent<HTMLButtonElement>) => void; active?: boolean; disabled?: boolean; big?: boolean; title?: string; color?: string };
function Btn({ icon, label, onClick, active, disabled, big, title, color }: BtnProps) {
  return (
    <button className={(big ? 'rb-big' : 'rb-small') + (active ? ' active' : '')} onClick={onClick} disabled={disabled} title={title ?? label}>
      <Icon name={icon} size={big ? 22 : 14} color={color} />
      <span>{label}</span>
    </button>
  );
}
function Group({ label, children }: { label: string; children: React.ReactNode }) {
  return <div className="rb-group"><div className="rb-items">{children}</div><div className="rb-label">{label}</div></div>;
}

export function Ribbon({ studio, layout, togglePanel, openDialog }: { studio: Studio; layout: Layout; togglePanel: (id: string) => void; openDialog: (d: string) => void }) {
  useSignals([studio.changed, studio.active?.selection.changed, studio.output.changed]);
  const [tab, setTab] = useState('Home');
  const [popup, setPopupState] = useState<null | { kind: 'color' | 'material' | 'part'; x: number; y: number }>(null);
  const openPopup = (kind: 'color' | 'material' | 'part', e: React.MouseEvent<HTMLButtonElement>) => {
    if (popup?.kind === kind) { setPopupState(null); return; }
    const r = e.currentTarget.getBoundingClientRect();
    setPopupState({ kind, x: r.left, y: r.bottom + 2 });
  };
  const setPopup = (_v: null) => setPopupState(null);
  useEffect(() => {
    if (!popup) return;
    const close = (e: MouseEvent) => { if (!(e.target as HTMLElement).closest('.rb-popup, .rb-popup-wrap')) setPopupState(null); };
    window.addEventListener('mousedown', close);
    return () => window.removeEventListener('mousedown', close);
  }, [popup]);
  const pstyle = popup ? { position: 'fixed' as const, left: popup.x, top: popup.y } : undefined;
  const doc = studio.active;
  const playing = !!studio.play;
  const noDoc = !doc;
  const editDisabled = noDoc || playing;
  const hasSel = !!doc?.selection.size;
  const s = doc?.settings;
  const tool = (tl: Tool, icon: string, label: string, key: string) => <Btn big icon={icon} label={t(label)} active={studio.tool === tl && !playing} disabled={editDisabled} onClick={() => studio.setTool(tl)} title={`${t(label)} (${key})`} />;
  const setSnap = (patch: Partial<NonNullable<typeof s>>) => { if (doc) { Object.assign(doc.settings, patch); doc.history.touch(); studio.viewport?.updateGizmo(); studio.emit(); } };
  const visible = (id: string) => !layout.hidden.includes(id);

  const snapGroup = (
    <Group label={t('Snap to Grid')}>
      <div className="rb-col">
        <label className="rb-field"><input type="checkbox" checked={!!s?.snapEnabled} disabled={noDoc} onChange={(e) => setSnap({ snapEnabled: e.target.checked })} /> {t('Move')}
          <input type="number" min={0.01} step={0.25} value={s?.moveSnap ?? 1} disabled={noDoc} onChange={(e) => setSnap({ moveSnap: Math.max(0.01, +e.target.value || 1) })} /> {t('studs')}</label>
        <label className="rb-field"><span className="rb-indent" />{t('Rotate')}
          <input type="number" min={1} max={180} value={s?.rotateSnap ?? 15} disabled={noDoc} onChange={(e) => setSnap({ rotateSnap: Math.max(1, +e.target.value || 15) })} /> °</label>
        <button className="rb-small" disabled={noDoc} onClick={() => { studio.viewport?.setSpace(studio.viewport.space === 'world' ? 'local' : 'world'); studio.emit(); }} title={t('Transform space (Ctrl+L)')}>
          <Icon name={studio.viewport?.space === 'local' ? 'local' : 'world'} size={14} /><span>{studio.viewport?.space === 'local' ? t('Local space') : t('World space')}</span>
        </button>
      </div>
    </Group>
  );
  const testGroup = (
    <Group label={t('Test')}>
      {!playing && <Btn big icon="play" label={t('Play')} color="#4ade80" disabled={noDoc} onClick={() => studio.startPlay()} title={t('Play (F5)')} />}
      {playing && <Btn big icon={studio.play!.paused ? 'play' : 'pause'} label={studio.play!.paused ? 'Resume' : 'Pause'} color="#facc15" onClick={() => studio.togglePause()} title={t('Pause / Resume (F5)')} />}
      <Btn big icon="stop" label={t('Stop')} color="#f87171" disabled={!playing} onClick={() => studio.stopPlay()} title={t('Stop (Shift+F5)')} />
      <div className="rb-col">
        <Btn icon="restart" label={t('Restart')} disabled={!playing} onClick={() => studio.restartPlay()} />
        {playing && <span className={'rb-status' + (studio.play!.errors ? ' err' : '')}><Icon name={studio.play!.errors ? 'error' : 'check'} size={12} /> {t('{n} errors', { n: studio.play!.errors })}</span>}
      </div>
    </Group>
  );

  const tabs: Record<string, React.ReactNode> = {
    Home: <>
      <Group label={t('Clipboard')}>
        <Btn big icon="paste" label={t('Paste')} disabled={editDisabled || !studio.clipboard} onClick={() => studio.paste()} />
        <div className="rb-col">
          <Btn icon="copy" label={t('Copy')} disabled={!hasSel} onClick={() => studio.copy()} />
          <Btn icon="cut" label={t('Cut')} disabled={editDisabled || !hasSel} onClick={() => studio.cut()} />
          <Btn icon="duplicate" label={t('Duplicate')} disabled={editDisabled || !hasSel} onClick={() => studio.duplicate()} />
        </div>
      </Group>
      <Group label={t('Tools')}>{tool('select', 'select', 'Select', 'Ctrl+1')}{tool('move', 'move', 'Move', 'Ctrl+2')}{tool('scale', 'scale', 'Scale', 'Ctrl+3')}{tool('rotate', 'rotate', 'Rotate', 'Ctrl+4')}</Group>
      {snapGroup}
      <Group label={t('Terrain')}><Btn big icon="mountain" label={t('Editor')} active={studio.tool === 'terrain'} disabled={editDisabled} onClick={() => { studio.setTool(studio.tool === 'terrain' ? 'select' : 'terrain'); }} /></Group>
      <Group label="World">
        <Btn big icon="road" label="Road" active={studio.tool === 'road' && !playing} disabled={editDisabled} title="Draw roads: click points, Enter / double-click to finish" onClick={() => studio.setTool(studio.tool === 'road' ? 'select' : 'road')} />
        <Btn big icon="building" label="Import" disabled={false} title="Import a place from OpenStreetMap" onClick={() => studio.panelRequest.fire('dialog:import-place')} />
      </Group>
      <Group label={t('Insert')}>
        <div className="rb-popup-wrap">
          <Btn big icon="part" label={t('Part ▾')} disabled={editDisabled} onClick={(e) => openPopup('part', e)} />
          {popup?.kind === 'part' && <div className="rb-popup" style={pstyle} onMouseLeave={() => setPopup(null)}>
            {([['Block', 'part'], ['Ball', 'ball'], ['Cylinder', 'cylinder'], ['Wedge', 'wedge']] as const).map(([n, ic]) => (
              <button key={n} className="rb-small" onClick={() => { setPopup(null); studio.insertShape(n); }}><Icon name={ic} size={14} /><span>{n}</span></button>
            ))}
          </div>}
        </div>
        <Btn big icon="plus" label={t('Toolbox')} active={visible('toolbox')} onClick={() => togglePanel('toolbox')} title={t('Show the Toolbox – everything you can insert')} />
        <div className="rb-col">
          <Btn icon="screengui" label={t('UI')} disabled={editDisabled} onClick={() => studio.insertObject('ScreenGui')} />
          <Btn icon="instance" label={t('All classes…')} disabled={editDisabled} onClick={() => openDialog('insert')} />
          <Btn icon="library" label={t('Assets')} onClick={() => togglePanel('assets')} active={visible('assets')} />
        </div>
      </Group>
      <Group label={t('Edit')}>
        <div className="rb-popup-wrap">
          <Btn big icon="color" label={t('Color ▾')} disabled={editDisabled || !hasSel} onClick={(e) => openPopup('color', e)} />
          {popup?.kind === 'color' && <div className="rb-popup palette" style={pstyle} onMouseLeave={() => setPopup(null)}>
            {PALETTE.map((c) => <button key={c} className="pal" style={{ background: c }} title={c} onClick={() => { setPopup(null); studio.setPropOnSelection('Color', c); }} />)}
          </div>}
        </div>
        <div className="rb-popup-wrap">
          <Btn big icon="material" label={t('Material ▾')} disabled={editDisabled || !hasSel} onClick={(e) => openPopup('material', e)} />
          {popup?.kind === 'material' && <div className="rb-popup materials" style={pstyle} onMouseLeave={() => setPopup(null)}>
            {MATERIALS.map((m) => { const u = materialPreviewUrl(m); return (
              <button key={m} className="mat" onClick={() => { setPopup(null); studio.setPropOnSelection('Material', m); }}>
                <span className="mat-swatch" style={{ backgroundImage: u ? `url(${u})` : undefined, backgroundColor: m === 'Neon' ? '#7fdcff' : '#9aa0a8' }} />{m}
              </button>); })}
          </div>}
        </div>
        <div className="rb-col">
          <Btn icon="group" label={t('Group')} disabled={editDisabled || !hasSel} onClick={() => studio.group()} />
          <Btn icon="lock" label={t('Lock')} disabled={editDisabled || !hasSel} onClick={() => studio.toggleProp('Locked')} />
          <Btn icon="anchor" label={t('Anchor')} disabled={editDisabled || !hasSel} onClick={() => studio.toggleProp('Anchored')} />
        </div>
      </Group>
      {testGroup}
    </>,
    Model: <>
      <Group label={t('Tools')}>{tool('select', 'select', 'Select', 'Ctrl+1')}{tool('move', 'move', 'Move', 'Ctrl+2')}{tool('scale', 'scale', 'Scale', 'Ctrl+3')}{tool('rotate', 'rotate', 'Rotate', 'Ctrl+4')}</Group>
      {snapGroup}
      <Group label={t('Parts')}>
        <div className="rb-col">
          <Btn icon="part" label={t('Block')} disabled={editDisabled} onClick={() => studio.insertShape('Block')} />
          <Btn icon="ball" label={t('Sphere')} disabled={editDisabled} onClick={() => studio.insertShape('Ball')} />
          <Btn icon="cylinder" label={t('Cylinder')} disabled={editDisabled} onClick={() => studio.insertShape('Cylinder')} />
        </div>
        <div className="rb-col">
          <Btn icon="wedge" label={t('Wedge')} disabled={editDisabled} onClick={() => studio.insertShape('Wedge')} />
          <Btn icon="spawn" label={t('Spawn')} disabled={editDisabled} onClick={() => studio.insertObject('SpawnLocation')} />
          <Btn icon="camera" label={t('Camera')} disabled={editDisabled} onClick={() => studio.insertObject('Camera')} />
        </div>
      </Group>
      <Group label={t('Effects')}>
        <div className="rb-col">
          <Btn icon="pointlight" label={t('Point Light')} disabled={editDisabled} onClick={() => studio.insertObject('PointLight')} />
          <Btn icon="spotlight" label={t('Spot Light')} disabled={editDisabled} onClick={() => studio.insertObject('SpotLight')} />
          <Btn icon="particles" label={t('Particles')} disabled={editDisabled} onClick={() => studio.insertObject('ParticleEmitter')} />
        </div>
        <div className="rb-col">
          <Btn icon="sound" label={t('Sound')} disabled={editDisabled} onClick={() => studio.insertObject('Sound')} />
          <Btn icon="animation" label={t('Animation')} disabled={editDisabled} onClick={() => studio.insertObject('KeyframeAnimation')} />
          <Btn icon="click" label={t('ClickDetector')} disabled={editDisabled} onClick={() => studio.insertObject('ClickDetector')} />
        </div>
      </Group>
      <Group label={t('Scripts')}>
        <div className="rb-col">
          <Btn icon="script" label={t('Script')} disabled={editDisabled} onClick={() => studio.insertObject('Script')} />
          <Btn icon="localscript" label={t('LocalScript')} disabled={editDisabled} onClick={() => studio.insertObject('LocalScript')} />
          <Btn icon="modulescript" label={t('ModuleScript')} disabled={editDisabled} onClick={() => studio.insertObject('ModuleScript')} />
        </div>
      </Group>
      <Group label={t('Physics & Grouping')}>
        <div className="rb-col">
          <Btn icon="anchor" label={t('Anchor')} disabled={editDisabled || !hasSel} onClick={() => studio.toggleProp('Anchored')} />
          <Btn icon="collide" label={t('Can Collide')} disabled={editDisabled || !hasSel} onClick={() => studio.toggleProp('CanCollide')} />
          <Btn icon="eye" label={t('Visible')} disabled={editDisabled || !hasSel} onClick={() => studio.toggleProp('Visible')} />
        </div>
        <div className="rb-col">
          <Btn icon="group" label={t('Group')} disabled={editDisabled || !hasSel} onClick={() => studio.group()} />
          <Btn icon="ungroup" label={t('Ungroup')} disabled={editDisabled || !hasSel} onClick={() => studio.ungroup()} />
          <Btn icon="library" label={t('Save to Library')} disabled={editDisabled || !hasSel} onClick={() => studio.saveSelectionAsAsset()} />
        </div>
      </Group>
    </>,
    Test: <>
      {testGroup}
      <Group label={t('Diagnostics')}>
        <Btn big icon="bug" label={t('Run Self-Test')} disabled={playing} onClick={() => studio.runSelfTest()} />
        <div className="rb-col">
          <Btn icon="terminal" label={t('Output')} active={visible('output')} onClick={() => togglePanel('output')} />
          <Btn icon="script" label={t('Script Analysis')} active={visible('analysis')} onClick={() => togglePanel('analysis')} />
          <Btn icon="stats" label={t('Stats')} active={studio.showStats} onClick={() => { studio.showStats = !studio.showStats; studio.emit(); }} />
        </div>
      </Group>
    </>,
    View: <>
      <Group label={t('Panels')}>
        <div className="rb-col">
          <Btn icon="workspace" label={t('Explorer')} active={visible('explorer')} onClick={() => togglePanel('explorer')} />
          <Btn icon="layout" label={t('Properties')} active={visible('properties')} onClick={() => togglePanel('properties')} />
          <Btn icon="terminal" label={t('Output')} active={visible('output')} onClick={() => togglePanel('output')} />
        </div>
        <div className="rb-col">
          <Btn icon="plus" label={t('Toolbox')} active={visible('toolbox')} onClick={() => togglePanel('toolbox')} />
          <Btn icon="library" label={t('Assets')} active={visible('assets')} onClick={() => togglePanel('assets')} />
          <Btn icon="mountain" label={t('Terrain Editor')} active={visible('terrain')} onClick={() => togglePanel('terrain')} />
          <Btn icon="script" label={t('Script Analysis')} active={visible('analysis')} onClick={() => togglePanel('analysis')} />
        </div>
      </Group>
      <Group label={t('Viewport')}>
        <Btn big icon="grid" label={t('Grid')} active={studio.showGrid} onClick={() => studio.toggleGrid()} />
        <Btn big icon="stats" label={t('Stats')} active={studio.showStats} onClick={() => { studio.showStats = !studio.showStats; studio.emit(); }} />
        <Btn big icon="focus" label={t('Focus')} disabled={!hasSel} onClick={() => studio.focusSelection()} title={t('Focus selection (F)')} />
      </Group>
      <Group label={t('Layout')}><Btn big icon="layout" label={t('Reset Layout')} onClick={() => openDialog('resetLayout')} /></Group>
    </>,
    Terrain: <>
      <Group label={t('Terrain')}>
        <Btn big icon="brush" label={t('Sculpt')} active={studio.tool === 'terrain'} disabled={editDisabled} onClick={() => studio.setTool('terrain')} />
        <Btn big icon="mountain" label={t('Generate')} disabled={editDisabled} onClick={() => togglePanel('terrain')} />
      </Group>
      {testGroup}
    </>,
  };

  return (
    <div className="ribbon">
      <div className="ribbon-tabs">
        {Object.keys(tabs).map((name) => <button key={name} className={tab === name ? 'active' : ''} onClick={() => setTab(name)}>{t(name).toUpperCase()}</button>)}
        <div className="spacer" />
        <button className="ribbon-quick" title={t('Undo (Ctrl+Z)')} disabled={editDisabled || !doc?.history.canUndo} onClick={() => studio.undo()}><Icon name="undo" size={14} /></button>
        <button className="ribbon-quick" title={t('Redo (Ctrl+Y)')} disabled={editDisabled || !doc?.history.canRedo} onClick={() => studio.redo()}><Icon name="redo" size={14} /></button>
        <button className="ribbon-quick" title={t('Save (Ctrl+S)')} disabled={noDoc} onClick={() => studio.save()}><Icon name="save" size={14} /></button>
      </div>
      <div className="ribbon-body">{tabs[tab]}</div>
    </div>
  );
}

// Terrain editor: brush tools, material, brush size/strength and generation.

import { useState } from 'react';
import type { Studio } from '../../editor/Studio';
import { BRUSH_TOOLS, generateTerrain, type TerrainPreset } from '../../terrain/TerrainTools';
import { TERRAIN_MATERIALS } from '../../terrain/TerrainData';
import { Icon } from '../icons';
import { useSignals } from '../hooks';

import { t } from '../../i18n';
export function TerrainPanel({ studio }: { studio: Studio }) {
  useSignals([studio.changed]);
  const vp = studio.viewport;
  const doc = studio.editDoc;
  const [, setTick] = useState(0);
  const [gen, setGen] = useState({ preset: 'hills' as TerrainPreset, seed: 1, amplitude: 40, scale: 120, baseHeight: 0, water: true, waterLevel: 4 });
  if (!vp) return null;
  if (!doc) return <div className="panel-empty">{t('Terrain can only be edited while no test is running.')}</div>;
  const b = vp.terrainBrush;
  const set = (patch: Partial<typeof b>) => { Object.assign(b, patch); setTick((x) => x + 1); };
  const terr = doc.dm.terrainInstance;
  const active = studio.tool === 'terrain';

  const modify = (label: string, fn: () => void) => {
    if (!terr?.terrain) return;
    const before = terr.terrain.snapshot();
    fn();
    const after = terr.terrain.snapshot();
    doc.transact(label, () => doc.history.record({ k: 'terrain', inst: terr, before, after }));
    doc.dm.terrainChanged.fire(terr, null);
  };

  return (
    <div className="terrain-panel">
      <div className="section">
        <div className="section-title">{t('Brush')}</div>
        {!active && <button className="btn primary wide" onClick={() => studio.setTool('terrain')}><Icon name="brush" size={13} /> {t('Activate terrain brush')}</button>}
        <div className="tool-grid">
          {BRUSH_TOOLS.map((bt) => (
            <button key={bt.id} className={'tool-btn' + (b.tool === bt.id && active ? ' active' : '')} title={t(bt.hint)}
              onClick={() => { set({ tool: bt.id }); if (!active) studio.setTool('terrain'); }}>{t(bt.label)}</button>
          ))}
        </div>
        <label className="slider-row">{t('Size')} <input type="range" min={2} max={80} value={b.radius} onChange={(e) => set({ radius: +e.target.value })} /><span>{b.radius}</span></label>
        <label className="slider-row">{t('Strength')} <input type="range" min={0.05} max={1} step={0.05} value={b.strength} onChange={(e) => set({ strength: +e.target.value })} /><span>{b.strength.toFixed(2)}</span></label>
        <div className="section-title">{t('Material')}</div>
        <div className="swatches">
          {TERRAIN_MATERIALS.filter((m) => m.id > 0).map((m) => (
            <button key={m.id} className={'swatch' + (b.material === m.id ? ' active' : '')} style={{ background: m.color }} title={t(m.name)} onClick={() => set({ material: m.id })}>
              <span>{t(m.name)}</span>
            </button>
          ))}
        </div>
        <p className="hint">{t('Left-drag in the viewport to sculpt. Water fills at Terrain.WaterLevel ({level}). Each stroke is one undo step.', { level: terr ? String(terr.props.WaterLevel) : '-' })}</p>
      </div>
      <div className="section">
        <div className="section-title">{t('Generate')}</div>
        <label className="field-row">{t('Type')}
          <select value={gen.preset} onChange={(e) => setGen({ ...gen, preset: e.target.value as TerrainPreset })}>
            <option value="hills">{t('Hills')}</option><option value="mountains">{t('Mountains')}</option><option value="valleys">{t('Valleys')}</option><option value="island">{t('Island')}</option><option value="flat">{t('Flat')}</option>
          </select>
        </label>
        <label className="field-row">{t('Seed')} <input type="number" value={gen.seed} onChange={(e) => setGen({ ...gen, seed: +e.target.value || 0 })} /></label>
        <label className="field-row">{t('Height')} <input type="number" value={gen.amplitude} onChange={(e) => setGen({ ...gen, amplitude: +e.target.value })} /></label>
        <label className="field-row">{t('Feature size')} <input type="number" value={gen.scale} min={10} onChange={(e) => setGen({ ...gen, scale: Math.max(10, +e.target.value) })} /></label>
        <label className="field-row">{t('Base height')} <input type="number" value={gen.baseHeight} onChange={(e) => setGen({ ...gen, baseHeight: +e.target.value })} /></label>
        <label className="field-row">{t('Water')} <input type="checkbox" checked={gen.water} onChange={(e) => setGen({ ...gen, water: e.target.checked })} /></label>
        {gen.water && <label className="field-row">{t('Water level')} <input type="number" value={gen.waterLevel} onChange={(e) => setGen({ ...gen, waterLevel: +e.target.value })} /></label>}
        <div className="btn-row">
          <button className="btn primary" onClick={() => modify('Generate terrain', () => {
            generateTerrain(terr!.terrain!, gen);
            if (gen.water) terr!.set('WaterLevel', gen.waterLevel);
          })}><Icon name="mountain" size={13} /> {t('Generate')}</button>
          <button className="btn" onClick={() => modify('Fill water', () => {
            const td = terr!.terrain!; const wl = terr!.props.WaterLevel as number;
            for (let i = 0; i < td.water.length; i++) td.water[i] = !td.materials[i] || td.heights[i] < wl ? 1 : 0;
          })}><Icon name="water" size={13} /> {t('Sea level')}</button>
          <button className="btn danger" onClick={() => modify('Clear terrain', () => terr!.terrain!.clear())}><Icon name="trash" size={13} /> {t('Clear')}</button>
        </div>
      </div>
    </div>
  );
}

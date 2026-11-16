// Dialog: import a place (terrain + buildings + roads + water) from OpenStreetMap / elevation data.

import { useState } from 'react';
import type { Studio } from '../editor/Studio';
import { Modal } from '../ui/Dialogs';
import { searchPlace, type PlaceResult } from './osm';
import { runImportPlace } from './WorldStudio';

export function ImportPlaceDialog({ studio, onClose }: { studio: Studio; onClose: () => void }) {
  const [q, setQ] = useState('');
  const [results, setResults] = useState<PlaceResult[]>([]);
  const [sel, setSel] = useState<PlaceResult | null>(null);
  const [radius, setRadius] = useState(500);
  const [terrain, setTerrain] = useState(true);
  const [buildings, setBuildings] = useState(true);
  const [roads, setRoads] = useState(true);
  const [water, setWater] = useState(true);
  const [areas, setAreas] = useState(false);
  const [mode, setMode] = useState<'new' | 'add'>('new');
  const [status, setStatus] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const hasDoc = !!studio.active;

  const search = async () => {
    setError(''); setStatus('Searching…');
    try {
      const r = await searchPlace(q);
      setResults(r); setSel(r[0] ?? null);
      setStatus(r.length ? '' : 'Nothing found.');
    } catch (e) { setError((e as Error).message); setStatus(''); }
  };
  const run = async () => {
    if (!sel) return;
    setBusy(true); setError('');
    try {
      await runImportPlace(studio, {
        name: sel.name.split(',')[0], lat: sel.lat, lon: sel.lon, radius, terrain, buildings, roads, water, areas,
        mode: mode === 'add' && hasDoc ? 'add' : 'new', target: studio.active ?? undefined,
      }, setStatus);
      onClose();
    } catch (e) { setError((e as Error).message); setBusy(false); setStatus(''); }
  };
  const check = (label: string, v: boolean, set: (b: boolean) => void, disabled = false) => (
    <label className="check-row"><input type="checkbox" checked={v} disabled={disabled || busy} onChange={(e) => set(e.target.checked)} /> {label}</label>
  );
  return (
    <Modal title="Import place" onClose={busy ? () => {} : onClose} width={560}>
      <div className="modal-body">
        <div style={{ display: 'flex', gap: 6 }}>
          <input className="text-input wide" autoFocus placeholder="Town, address or coordinates (48.2082, 16.3738)" value={q} disabled={busy}
            onChange={(e) => setQ(e.target.value)} onKeyDown={(e) => { if (e.key === 'Enter') search(); }} />
          <button className="btn" onClick={search} disabled={busy || !q.trim()}>Search</button>
        </div>
        <div className="class-list" style={{ maxHeight: 150, marginTop: 8 }}>
          {results.map((r, i) => (
            <div key={i} className={'class-item' + (sel === r ? ' active' : '')} onClick={() => setSel(r)}><b>{r.name.split(',')[0]}</b><span className="dim">{r.name}</span></div>
          ))}
        </div>
        <div style={{ marginTop: 10 }}>
          <label>Radius: <b>{radius} m</b> <span className="dim">(larger = more objects; 300–800 m runs smoothly)</span></label>
          <input type="range" min={150} max={2000} step={50} value={radius} disabled={busy} onChange={(e) => setRadius(+e.target.value)} style={{ width: '100%' }} />
        </div>
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 4, marginTop: 6 }}>
          {check('Terrain (real elevation)', terrain, setTerrain, mode === 'add' && hasDoc)}
          {check('Buildings', buildings, setBuildings)}
          {check('Roads', roads, setRoads)}
          {check('Water', water, setWater)}
          {check('Parks / forests (flat areas)', areas, setAreas)}
        </div>
        {hasDoc && (
          <div style={{ marginTop: 8 }}>
            <label className="check-row"><input type="radio" checked={mode === 'new'} disabled={busy} onChange={() => setMode('new')} /> New project</label>
            <label className="check-row"><input type="radio" checked={mode === 'add'} disabled={busy} onChange={() => setMode('add')} /> Add to the current project (same origin)</label>
          </div>
        )}
        <p className="hint">Data: OpenStreetMap (© contributors, ODbL) and AWS Terrain Tiles. Everything imported becomes ordinary, editable objects. The game loads OpenStreetMap by itself, so the map repository only stores what you add or change.</p>
        {status && <p className="hint">{status}</p>}
        {error && <p className="hint" style={{ color: '#ff8a8a' }}>{error}</p>}
      </div>
      <div className="modal-buttons">
        <button className="btn" onClick={onClose} disabled={busy}>Cancel</button>
        <button className="btn primary" onClick={run} disabled={!sel || busy}>{busy ? 'Importing…' : 'Import'}</button>
      </div>
    </Modal>
  );
}

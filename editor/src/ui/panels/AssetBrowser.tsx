// Asset browser / toolbox: built-in library + project assets by category,
// with search, previews, import/export, insert/apply, rename and delete.

import { useEffect, useState } from 'react';
import type { Studio } from '../../editor/Studio';
import type { AssetRecord } from '../../core/Serializer';
import { listBuiltin } from '../../assets/builtin';
import { assetDataUrl, validateAnimation } from '../../assets/AssetStore';
import { PREFAB_THUMB_COLORS } from '../../assets/prefabs';
import { materialPreviewUrl } from '../../render/materials';
import { modelThumbnail } from '../../render/thumbnails';
import { previewSound } from '../../runtime/Audio';
import { Icon } from '../icons';
import { useSignals } from '../hooks';

import { msg, t } from '../../i18n';

const CATS: { id: AssetRecord['kind'] | 'all'; label: string; icon: string; project?: boolean }[] = [
  { id: 'all', label: msg('All'), icon: 'library' },
  { id: 'material', label: msg('Materials'), icon: 'material' },
  { id: 'texture', label: msg('Textures'), icon: 'imagelabel' },
  { id: 'sound', label: msg('Sounds'), icon: 'sound' },
  { id: 'animation', label: msg('Animations'), icon: 'animation' },
  { id: 'model', label: msg('Models'), icon: 'model', project: true },
  { id: 'script', label: msg('Scripts'), icon: 'script', project: true },
];

const HINT: Record<string, string> = {
  model: msg('Double-click to insert into the scene'),
  material: msg('Double-click to apply to the selected parts'),
  texture: msg('Double-click to apply to the selected parts (TextureId) or ImageLabels'),
  sound: msg('Double-click to add a Sound to the selection'),
  script: msg('Double-click to insert as a Script'),
  animation: msg('Double-click to add a KeyframeAnimation to the selected part/model'),
};

function Thumb({ a }: { a: AssetRecord }) {
  const [url, setUrl] = useState<string | null>(null);
  const [failed, setFailed] = useState(false);
  useEffect(() => {
    let alive = true;
    if (a.kind === 'texture') setUrl(assetDataUrl(a));
    else if (a.kind === 'material') setUrl(materialPreviewUrl(a.data));
    else if (a.kind === 'model') modelThumbnail(a).then((u) => alive && setUrl(u)).catch(() => alive && setFailed(true));
    return () => { alive = false; };
  }, [a]);
  if (a.kind === 'material') return <div className="thumb material" style={{ backgroundImage: url ? `url(${url})` : undefined, backgroundColor: a.data === 'Neon' ? '#7fdcff' : a.data === 'Glass' ? '#a9d8ff88' : '#8b8f96' }} />;
  if (url) return <div className="thumb" style={{ backgroundImage: `url(${url})` }} />;
  const icon = a.kind === 'model' ? 'model' : a.kind === 'sound' ? 'sound' : a.kind === 'script' ? 'script' : a.kind === 'animation' ? 'animation' : 'imagelabel';
  return <div className="thumb icon" style={{ color: PREFAB_THUMB_COLORS[a.name] }}>{failed ? <Icon name="warn" size={28} color="#f5c04a" /> : <Icon name={icon} size={30} />}</div>;
}

export function AssetBrowser({ studio }: { studio: Studio }) {
  const doc = studio.active;
  useSignals([studio.changed, doc?.assets.changed]);
  const [cat, setCat] = useState<(typeof CATS)[number]['id']>('all');
  const [source, setSource] = useState<'library' | 'project'>('library');
  const [query, setQuery] = useState('');
  const [selected, setSelected] = useState<string | null>(null);
  const [stopSound, setStopSound] = useState<{ id: string; stop: () => void } | null>(null);
  useEffect(() => () => stopSound?.stop(), [stopSound]);
  if (!doc) return <div className="panel-empty">{t('Open a project to browse assets.')}</div>;

  const effCat = source === 'library' && (cat === 'model' || cat === 'script') ? 'all' : cat;
  const all = source === 'library' ? listBuiltin().filter((a) => a.mime !== 'application/x-forge-prefab' && a.kind !== 'script') : doc.assets.list();
  const items = all.filter((a) => (effCat === 'all' || a.kind === effCat) && (!query || a.name.toLowerCase().includes(query.toLowerCase())));
  const sel = items.find((a) => a.id === selected) ?? null;

  const play = async (a: AssetRecord) => {
    if (stopSound?.id === a.id) { stopSound.stop(); setStopSound(null); return; }
    stopSound?.stop();
    try { const stop = await previewSound(doc.assets, a.id); setStopSound({ id: a.id, stop }); }
    catch (e) { studio.output.error(t('Cannot play "{name}": {error}', { name: a.name, error: (e as Error).message })); }
  };

  const menu = (e: React.MouseEvent, a: AssetRecord) => {
    e.preventDefault();
    setSelected(a.id);
    const project = source === 'project';
    studio.contextMenu.fire({ x: e.clientX, y: e.clientY, items: [
      { label: a.kind === 'model' || a.kind === 'script' ? t('Insert') : t('Apply / Add to selection'), action: () => studio.useAsset(a), disabled: !!studio.play },
      ...(a.kind === 'sound' ? [{ label: t('Preview'), action: () => play(a) }] : []),
      { label: t('Export to file…'), action: () => studio.exportAsset(a), disabled: a.kind === 'material' },
      ...(!project && a.kind !== 'material' ? [{ label: t('Copy into project'), action: () => { doc.assets.add({ name: a.name, kind: a.kind, mime: a.mime, data: a.data, encoding: a.encoding }); setSource('project'); } }] : []),
      ...(project ? [
        { separator: true },
        { label: t('Rename…'), action: async () => { const n = await studio.prompt(t('Rename asset'), t('New name:'), a.name); if (n) doc.assets.rename(a.id, n); } },
        { label: t('Delete'), action: async () => {
          let used = 0;
          doc.dm.walk((i) => { for (const v of Object.values(i.props)) if (v === a.id) used++; });
          const r = await studio.ask(t('Delete asset'), used ? t('"{name}" is used by {n} object(s). Delete anyway? Those references will show as missing.', { name: a.name, n: used }) : t('Delete "{name}" from the project?', { name: a.name }), [{ id: 'del', label: t('Delete'), danger: true }, { id: 'cancel', label: t('Cancel') }]);
          if (r.button === 'del') doc.assets.remove(a.id);
        } },
      ] : []),
    ] });
  };

  return (
    <div className="assets">
      <div className="panel-toolbar">
        <div className="segmented">
          <button className={source === 'library' ? 'on' : ''} onClick={() => setSource('library')}>{t('Library')}</button>
          <button className={source === 'project' ? 'on' : ''} onClick={() => setSource('project')}>{t('Project ({n})', { n: doc.assets.list().length })}</button>
        </div>
        <div className="spacer" />
        <button className="btn small" title={t('Import glTF/GLB, PNG/JPG, MP3/WAV/OGG, Lua, animation JSON, .forgemodel')} onClick={async () => { const r = await studio.importAssets(); if (r.length) { setSource('project'); setCat('all'); } }}><Icon name="importIcon" size={12} /> {t('Import')}</button>
      </div>
      <div className="panel-toolbar">
        <div className="search-box"><Icon name="search" size={13} /><input placeholder={t('Search assets')} value={query} onChange={(e) => setQuery(e.target.value)} /></div>
      </div>
      <div className="asset-cats">
        {CATS.filter((c) => !c.project || source === 'project').map((c) => <button key={c.id} className={effCat === c.id ? 'on' : ''} onClick={() => setCat(c.id)} title={t(c.label)}><Icon name={c.icon} size={13} /><span>{t(c.label)}</span></button>)}
      </div>
      <div className="asset-grid">
        {items.map((a) => (
          <div key={a.id} className={'asset-item' + (selected === a.id ? ' selected' : '')} title={`${source === 'library' ? t(a.name) : a.name}\n${t(HINT[a.kind])}`}
            onClick={() => setSelected(a.id)} onDoubleClick={() => studio.useAsset(a)} onContextMenu={(e) => menu(e, a)}
            draggable={false}>
            <Thumb a={a} />
            <div className="asset-name">{source === 'library' ? t(a.name) : a.name}</div>
            {a.kind === 'sound' && <button className="asset-play" onClick={(e) => { e.stopPropagation(); play(a); }}><Icon name={stopSound?.id === a.id ? 'stop' : 'play'} size={11} /></button>}
          </div>
        ))}
        {!items.length && <div className="panel-empty">{source === 'project' ? t('No project assets yet. Use Import, drag files here, or "Save to Asset Library" on a selection.') : t('Nothing matches.')}</div>}
      </div>
      {sel && <AssetDetails a={sel} />}
    </div>
  );
}

function AssetDetails({ a }: { a: AssetRecord }) {
  let detail = '';
  if (a.kind === 'script') detail = a.data.split('\n').slice(0, 6).join('\n');
  else if (a.kind === 'animation') { try { const c = validateAnimation(a.data); detail = t('{n} keyframes, {s} s', { n: c.keyframes.length, s: c.duration }); } catch (e) { detail = (e as Error).message; } }
  else if (a.encoding === 'base64') detail = `${a.mime} · ${(a.data.length * 0.75 / 1024).toFixed(1)} KB`;
  else detail = a.mime;
  return (
    <div className="asset-details">
      <div className="asset-details-title">{a.name} <span className="dim">({a.kind})</span></div>
      <pre>{detail}</pre>
    </div>
  );
}

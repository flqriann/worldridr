// Modal dialogs: generic ask/prompt host, Insert Object, Settings (incl.
// language packs), Shortcuts, About.

import { useEffect, useState } from 'react';
import type { DialogRequest, Studio } from '../editor/Studio';
import { creatableClasses } from '../core/classes';
import { ClassIcon, Icon } from './icons';
import { isElectron, native, openFiles, saveFile } from '../project/ProjectIO';
import { msg, currentLanguage, installLanguagePack, languagePackTemplate, languages, packCoverage, parseLanguagePack, removeLanguagePack, setLanguage, t } from '../i18n';

export function Modal({ title, onClose, children, width = 420 }: { title: string; onClose: () => void; children: React.ReactNode; width?: number }) {
  useEffect(() => {
    const k = (e: KeyboardEvent) => { if (e.key === 'Escape') onClose(); };
    window.addEventListener('keydown', k);
    return () => window.removeEventListener('keydown', k);
  }, [onClose]);
  return (
    <div className="modal-backdrop" onMouseDown={(e) => { if (e.target === e.currentTarget) onClose(); }}>
      <div className="modal" style={{ width }} role="dialog" aria-label={title}>
        <div className="modal-title">{title}<button className="icon-btn" onClick={onClose}><Icon name="close" size={12} /></button></div>
        {children}
      </div>
    </div>
  );
}

export function DialogHost({ studio }: { studio: Studio }) {
  const [queue, setQueue] = useState<DialogRequest[]>([]);
  const [value, setValue] = useState('');
  useEffect(() => studio.dialogs.connect((d) => { setQueue((q) => [...q, d]); }), [studio]);
  const cur = queue[0];
  useEffect(() => { if (cur?.input) setValue(cur.input.value); }, [cur]);
  if (!cur) return null;
  const done = (button: string) => { setQueue((q) => q.slice(1)); cur.resolve({ button, value }); };
  const cancelId = cur.buttons.find((b) => b.id === 'cancel')?.id ?? cur.buttons[cur.buttons.length - 1].id;
  return (
    <Modal title={cur.title} onClose={() => done(cancelId)}>
      <div className="modal-body">
        <p className="modal-message">{cur.message}</p>
        {cur.input && <input className="text-input wide" autoFocus value={value} placeholder={cur.input.placeholder} onChange={(e) => setValue(e.target.value)} onKeyDown={(e) => { if (e.key === 'Enter') done(cur.buttons.find((b) => b.primary)?.id ?? 'ok'); }} />}
      </div>
      <div className="modal-buttons">
        {cur.buttons.map((b) => <button key={b.id} autoFocus={!cur.input && b.primary} className={'btn' + (b.primary ? ' primary' : '') + (b.danger ? ' danger' : '')} onClick={() => done(b.id)}>{b.label}</button>)}
      </div>
    </Modal>
  );
}

export function InsertObjectDialog({ studio, onClose }: { studio: Studio; onClose: () => void }) {
  const [q, setQ] = useState('');
  const list = creatableClasses().filter((c) => c.name.toLowerCase().includes(q.toLowerCase()) || (c.insertCategory ?? '').toLowerCase().includes(q.toLowerCase()) || t(c.description).toLowerCase().includes(q.toLowerCase()));
  const [idx, setIdx] = useState(0);
  const pick = (name: string) => { onClose(); studio.insertObject(name); };
  return (
    <Modal title={t('Insert Object')} onClose={onClose} width={480}>
      <div className="modal-body">
        <input className="text-input wide" autoFocus placeholder={t('Search classes…')} value={q} onChange={(e) => { setQ(e.target.value); setIdx(0); }}
          onKeyDown={(e) => {
            if (e.key === 'ArrowDown') setIdx((i) => Math.min(list.length - 1, i + 1));
            if (e.key === 'ArrowUp') setIdx((i) => Math.max(0, i - 1));
            if (e.key === 'Enter' && list[idx]) pick(list[idx].name);
          }} />
        <div className="class-list">
          {list.map((c, i) => (
            <div key={c.name} className={'class-item' + (i === idx ? ' active' : '')} onMouseEnter={() => setIdx(i)} onClick={() => pick(c.name)}>
              <ClassIcon className={c.name} /><b>{c.name}</b><span className="dim">{t(c.description)}</span>
            </div>
          ))}
        </div>
        <p className="hint">{t('The object is inserted into the selected object, or into a sensible default container.')}</p>
      </div>
    </Modal>
  );
}

type SettingsTab = 'general' | 'language' | 'project';

function LanguageSettings({ studio }: { studio: Studio }) {
  const [, tick] = useState(0);
  const [error, setError] = useState<string | null>(null);
  const install = async () => {
    setError(null);
    try {
      const files = await openFiles('language', true);
      for (const f of files) {
        const pack = parseLanguagePack(new TextDecoder().decode(f.data));
        installLanguagePack(pack);
        studio.output.system(t('Installed language pack "{name}" ({code}).', { name: pack.name, code: pack.code }));
      }
      tick((x) => x + 1);
    } catch (e) { setError((e as Error).message); }
  };
  const exportTemplate = async (prefill: 'en' | 'de') => {
    try { await saveFile('language', `forge-language-template-${prefill}.json`, languagePackTemplate(prefill)); } catch (e) { setError((e as Error).message); }
  };
  return (
    <>
      <div className="section-title">{t('Interface language')}</div>
      <div className="lang-list">
        {languages().map((l) => (
          <label key={l.code} className={'lang-row' + (currentLanguage() === l.code ? ' active' : '')}>
            <input type="radio" name="lang" checked={currentLanguage() === l.code} onChange={() => setLanguage(l.code)} />
            <span className="lang-name">{l.name}</span>
            <span className="dim">{l.code}{l.author ? ` · ${l.author}` : ''}</span>
            <span className="lang-cov">{l.code === 'en' ? t('default') : `${Math.round(packCoverage(l) * 100)} %`}</span>
            {!l.builtin && l.source !== 'folder' && <button className="icon-btn" title={t('Remove language pack')} onClick={(e) => { e.preventDefault(); removeLanguagePack(l.code); tick((x) => x + 1); }}><Icon name="trash" size={12} /></button>}
            {l.source === 'folder' && <span className="badge">{t('folder')}</span>}
          </label>
        ))}
      </div>
      <div className="btn-row">
        <button className="btn" onClick={install}><Icon name="importIcon" size={12} /> {t('Install language pack…')}</button>
        <button className="btn" onClick={() => exportTemplate('en')}><Icon name="exportIcon" size={12} /> {t('Export template (English)')}</button>
        <button className="btn" onClick={() => exportTemplate('de')}><Icon name="exportIcon" size={12} /> {t('Export template (German)')}</button>
        {isElectron && <button className="btn" onClick={() => native!.languageOpenFolder()}><Icon name="open" size={12} /> {t('Open languages folder')}</button>}
      </div>
      {error && <div className="status-box error">{error}</div>}
      <p className="hint">{t('A language pack is a JSON file that maps the English interface texts to your language. Export a template, translate the values, set "code" and "name", then install it here.')}{isElectron ? ' ' + t('Packs placed in the languages folder are loaded automatically at startup.') : ''}</p>
    </>
  );
}

export function SettingsDialog({ studio, onClose }: { studio: Studio; onClose: () => void }) {
  const doc = studio.active;
  const [tab, setTab] = useState<SettingsTab>('general');
  const [s, setS] = useState(() => ({ ...(doc?.settings ?? { gridSize: 4, moveSnap: 1, rotateSnap: 15, snapEnabled: true, description: '' }) }));
  const [auto, setAuto] = useState(studio.autosaveMinutes);
  const [name, setName] = useState(doc?.name ?? '');
  const apply = () => {
    if (doc) {
      Object.assign(doc.settings, s);
      if (name.trim() && name !== doc.name) doc.rename(name.trim());
      else doc.history.touch();
      studio.viewport?.updateGrid();
      studio.viewport?.updateGizmo();
    }
    studio.setAutosaveMinutes(auto);
    onClose();
  };
  return (
    <Modal title={t('Settings')} onClose={onClose} width={560}>
      <div className="settings-tabs">
        <button className={tab === 'general' ? 'on' : ''} onClick={() => setTab('general')}>{t('General')}</button>
        <button className={tab === 'language' ? 'on' : ''} onClick={() => setTab('language')}>{t('Language')}</button>
        {doc && <button className={tab === 'project' ? 'on' : ''} onClick={() => setTab('project')}>{t('Project')}</button>}
      </div>
      <div className="modal-body form">
        {tab === 'general' && <>
          <div className="section-title">{t('Studio')}</div>
          <label className="field-row">{t('Autosave every (min, 0 = off)')} <input type="number" min={0} max={60} value={auto} onChange={(e) => setAuto(Math.max(0, +e.target.value))} /></label>
          <p className="hint">{isElectron ? t('Autosaves are stored in the app data folder and offered for recovery after a crash.') : t('Autosaves are stored in the browser (IndexedDB) and offered for recovery after a crash.')}</p>
        </>}
        {tab === 'language' && <LanguageSettings studio={studio} />}
        {tab === 'project' && doc && <>
          <label className="field-row">{t('Name')} <input className="text-input" value={name} onChange={(e) => setName(e.target.value)} /></label>
          <label className="field-row">{t('Description')} <input className="text-input" value={s.description} onChange={(e) => setS({ ...s, description: e.target.value })} /></label>
          <label className="field-row">{t('Grid size (studs)')} <input type="number" min={0.25} step={0.25} value={s.gridSize} onChange={(e) => setS({ ...s, gridSize: Math.max(0.25, +e.target.value) })} /></label>
          <label className="field-row">{t('Snap enabled')} <input type="checkbox" checked={s.snapEnabled} onChange={(e) => setS({ ...s, snapEnabled: e.target.checked })} /></label>
          <label className="field-row">{t('Move / scale snap')} <input type="number" min={0.01} step={0.25} value={s.moveSnap} onChange={(e) => setS({ ...s, moveSnap: Math.max(0.01, +e.target.value) })} /></label>
          <label className="field-row">{t('Rotate snap (°)')} <input type="number" min={1} max={180} value={s.rotateSnap} onChange={(e) => setS({ ...s, rotateSnap: Math.max(1, +e.target.value) })} /></label>
          {doc.settings.publish && <p className="hint">{t('Published to {server} as game {id} (version {v}).', { server: doc.settings.publish.server, id: doc.settings.publish.gameId, v: doc.settings.publish.version })}</p>}
        </>}
      </div>
      <div className="modal-buttons"><button className="btn primary" onClick={apply}>{t('Apply')}</button><button className="btn" onClick={onClose}>{t('Cancel')}</button></div>
    </Modal>
  );
}

const SHORTCUTS: [string, string][] = [
  [msg('Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S'), msg('New / Open / Save / Save to File')],
  [msg('Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z)'), msg('Undo / Redo')],
  [msg('Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+Shift+V'), msg('Copy / Cut / Paste / Paste Into')],
  [msg('Ctrl+D / Delete / F2'), msg('Duplicate / Delete / Rename')],
  [msg('Ctrl+G / Ctrl+Alt+G / Ctrl+U'), msg('Group as Model / Folder / Ungroup')],
  [msg('Ctrl+1 / 2 / 3 / 4'), msg('Select / Move / Scale / Rotate tool')],
  [msg('Ctrl+L'), msg('Toggle local/world transform space')],
  [msg('Ctrl+A / Esc'), msg('Select all / Clear selection')],
  [msg('F'), msg('Focus camera on selection')],
  [msg('Right mouse drag'), msg('Look around (viewport)')],
  [msg('Middle mouse drag'), msg('Pan')],
  [msg('Mouse wheel'), msg('Zoom toward cursor')],
  [msg('W A S D / Q E (Shift = slow)'), msg('Fly camera (viewport focused)')],
  [msg('Left drag on a part (any tool)'), msg('Drag-move along surfaces')],
  [msg('R / T while dragging'), msg('Rotate 90° / tilt 90°')],
  [msg('Left drag on empty space'), msg('Box selection')],
  [msg('Ctrl/Shift+Click, Alt+Click'), msg('Add to selection, select part inside model')],
  [msg('Arrow keys (UI editor)'), msg('Nudge selected UI by 1 px (Shift: 10 px)')],
  [msg('F5 / Shift+F5 (F8)'), msg('Play or pause/resume / Stop test')],
  [msg('Script editor: Ctrl+F / Ctrl+H / Shift+Alt+F'), msg('Find / Replace / Format')],
];

export function ShortcutsDialog({ onClose }: { onClose: () => void }) {
  return (
    <Modal title={t('Keyboard & Mouse')} onClose={onClose} width={600}>
      <div className="modal-body"><table className="shortcut-table"><tbody>{SHORTCUTS.map(([k, d]) => <tr key={k}><td><kbd>{t(k)}</kbd></td><td>{t(d)}</td></tr>)}</tbody></table></div>
    </Modal>
  );
}

export function AboutDialog({ onClose }: { onClose: () => void }) {
  return (
    <Modal title={t('About Forge Studio')} onClose={onClose} width={520}>
      <div className="modal-body about">
        <p><b>WorldRidr Studio</b> – world editor for worldridr, built on Forge Studio (Roblox-Studio-style workflow).</p>
        <p>{t('Built with Electron, React, Three.js (rendering), Rapier (Rust physics, WASM), a Rust core for terrain (WASM), a Rust game server (axum/tokio), Monaco (code editor) and Lua 5.4 (WebAssembly).')}</p>
        <p className="hint">{t('Scripts run in standard Lua 5.4 with a Roblox-like API and a few Luau conveniences (compound assignment like x += 1). It is not fully Luau-compatible: type annotations, continue and string interpolation are not supported. See docs/SCRIPTING_API.md.')}</p>
        <p className="hint">{t('Not affiliated with or endorsed by Roblox Corporation. All icons, textures and sounds are original or procedurally generated.')}</p>
      </div>
    </Modal>
  );
}

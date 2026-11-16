// Main window. On the start screen only a slim title bar and the Home page
// are shown; with a project open: menu bar, ribbon, project tabs, dock
// layout with panels, center area (3D viewport + script tabs), status bar.

import { useEffect, useRef, useState } from 'react';
import type { Studio } from '../editor/Studio';
import { insertMenuItems, type ContextMenuItem } from '../editor/Studio';
import { Viewport } from '../render/Viewport';
import { DockLayout, DEFAULT_LAYOUT, hidePanel, loadLayout, saveLayout, showPanel, type Layout, type PanelDef } from './Dock';
import { MenuBar, ContextMenuHost } from './Menu';
import { ImportPlaceDialog } from '../world/ImportDialog';
import { cloneMap, commitAndPush, mapOptions, openMapFolder, pullMap, saveMap } from '../world/WorldStudio';
import { Ribbon } from './Ribbon';
import { Explorer } from './panels/Explorer';
import { Properties } from './panels/Properties';
import { Output } from './panels/Output';
import { AssetBrowser } from './panels/AssetBrowser';
import { Toolbox } from './panels/Toolbox';
import { TerrainPanel } from './panels/TerrainPanel';
import { ScriptAnalysis } from './panels/ScriptAnalysis';
import { ScriptEditor, runEditorAction } from './ScriptEditor';
import { Home } from './Home';
import { AboutDialog, DialogHost, InsertObjectDialog, Modal, SettingsDialog, ShortcutsDialog } from './Dialogs';
import { PublishDialog, VersionsDialog } from './PublishDialogs';
import apiDoc from '../../docs/SCRIPTING_API.md?raw';
import { ClassIcon, Icon } from './icons';
import { useSignals } from './hooks';
import { EXAMPLES, TEMPLATES } from '../project/templates';
import { isElectron, native } from '../project/ProjectIO';
import { PREFAB_NAMES } from '../assets/builtin';
import { languageChanged, t } from '../i18n';

function SceneHost({ studio }: { studio: Studio }) {
  const ref = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const el = ref.current!;
    if (!studio.viewport) {
      const holder = document.createElement('div');
      holder.className = 'viewport-holder';
      const vp = new Viewport(holder);
      studio.attachViewport(vp);
    }
    el.appendChild(studio.viewport!.container);
    studio.viewport!.resize();
  }, [studio]);
  return <div className="scene-host" ref={ref}><Stats studio={studio} /></div>;
}

function Stats({ studio }: { studio: Studio }) {
  const [, set] = useState(0);
  useSignals([studio.changed]);
  useEffect(() => { if (!studio.showStats) return; const id = setInterval(() => set((x) => x + 1), 500); return () => clearInterval(id); }, [studio.showStats]);
  const vp = studio.viewport;
  if (!studio.showStats || !vp) return null;
  const mem = (performance as unknown as { memory?: { usedJSHeapSize: number } }).memory;
  return (
    <div className="stats-overlay">
      <div>FPS <b>{vp.stats.fps}</b></div>
      <div>{t('Frame')} <b>{vp.stats.frameMs.toFixed(1)} ms</b></div>
      <div>{t('Draw calls')} <b>{vp.stats.drawCalls}</b></div>
      <div>{t('Triangles')} <b>{vp.stats.triangles.toLocaleString()}</b></div>
      <div>{t('Parts')} <b>{vp.partCount}</b></div>
      {mem && <div>JS heap <b>{(mem.usedJSHeapSize / 1048576).toFixed(0)} MB</b></div>}
    </div>
  );
}

function Center({ studio }: { studio: Studio }) {
  const doc = studio.active;
  useSignals([studio.changed]);
  if (!doc) return <div className="center" />;
  const tabs = [{ id: 'scene', label: doc.name, icon: 'workspace' }, ...doc.openScripts.map((id) => { const i = doc.dm.byId(id); return { id, label: i?.name ?? '?', icon: i?.className ?? 'Script', cls: true }; })];
  const active = doc.activeTab;
  return (
    <div className="center">
      <div className="inner-tabs">
        {tabs.map((tab) => (
          <div key={tab.id} className={'inner-tab' + (tab.id === active ? ' active' : '')} onClick={() => doc.setActiveTab(tab.id)} onMouseDown={(e) => { if (e.button === 1 && tab.id !== 'scene') { e.preventDefault(); doc.closeScript(tab.id); } }}>
            {'cls' in tab ? <ClassIcon className={tab.icon} /> : <Icon name={tab.icon} size={13} />}
            <span>{tab.label}</span>
            {tab.id !== 'scene' && <button className="tab-close" onClick={(e) => { e.stopPropagation(); doc.closeScript(tab.id); }}><Icon name="close" size={10} /></button>}
          </div>
        ))}
        {studio.play && <div className="play-banner"><span className="dot" /> {studio.play.paused ? t('PAUSED') : t('PLAYING')} – {t('changes are temporary')}</div>}
      </div>
      <div className="center-body">
        <div className="center-layer" style={{ display: active === 'scene' ? 'flex' : 'none' }}><SceneHost studio={studio} /></div>
        {active !== 'scene' && <div className="center-layer"><ScriptEditor key={doc.id} studio={studio} doc={doc} scriptId={active} /></div>}
      </div>
    </div>
  );
}

function DocTabs({ studio, home }: { studio: Studio; home?: boolean }) {
  useSignals([studio.changed]);
  return (
    <div className={'doc-tabs' + (home ? ' in-title' : '')}>
      {!home && <div className={'doc-tab home' + (studio.showStart ? ' active' : '')} onClick={() => studio.showStartPage()} title={t('Start page')}><Icon name="layout" size={13} /></div>}
      {studio.docs.map((d) => (
        <div key={d.id} className={'doc-tab' + (d === studio.active && !studio.showStart ? ' active' : '')} onClick={() => studio.activate(d)} title={d.filePath ?? t('Not saved yet')}
          onMouseDown={(e) => { if (e.button === 1) { e.preventDefault(); studio.closeDoc(d); } }}>
          <Icon name="game" size={13} /><span>{d.title}</span>
          <button className="tab-close" onClick={(e) => { e.stopPropagation(); studio.closeDoc(d); }}><Icon name="close" size={10} /></button>
        </div>
      ))}
      {!home && <button className="doc-tab-add" title={t('New project (Ctrl+N)')} onClick={() => studio.newProject()}><Icon name="plus" size={13} /></button>}
    </div>
  );
}

function StatusBar({ studio }: { studio: Studio }) {
  const doc = studio.active;
  useSignals([studio.changed, doc?.selection.changed, studio.output.changed]);
  const sel = studio.play ? studio.play.selection : doc?.selection;
  const last = studio.output.entries.at(-1);
  const toolNames: Record<string, string> = { select: t('Select'), move: t('Move'), scale: t('Scale'), rotate: t('Rotate'), terrain: t('Terrain') };
  return (
    <div className="statusbar">
      <span>{studio.play ? (studio.play.paused ? '⏸ ' + t('Paused') : '▶ ' + t('Running')) : t('Edit mode')}</span>
      {doc && <span>{t('Tool')}: {toolNames[studio.tool]}</span>}
      {doc && <span>{t('{n} selected', { n: sel?.size ?? 0 })}</span>}
      {doc && <span>{t('{n} objects', { n: (studio.play?.dm ?? doc.dm).index.size })}</span>}
      {doc && <span>{doc.dirty ? t('Unsaved changes') : t('Saved')}</span>}
      {studio.published && <span title={studio.published.server}><Icon name="world" size={11} /> {t('Published v{n}', { n: studio.published.version })}</span>}
      <span className="status-msg" title={last?.message}>{last ? last.message.split('\n')[0] : ''}</span>
      <span className={studio.output.errorCount ? 'err' : ''}><Icon name="error" size={11} /> {studio.output.errorCount}</span>
      <span><Icon name="warn" size={11} /> {studio.output.warnCount}</span>
      <span>{isElectron ? t('Desktop') : t('Browser')}</span>
    </div>
  );
}

function TitleBar({ studio, openSettings }: { studio: Studio; openSettings: () => void }) {
  return (
    <div className="titlebar">
      <div className="titlebar-brand"><svg width="20" height="20" viewBox="0 0 16 16"><path d="M8 1 14.5 4.8v6.4L8 15 1.5 11.2V4.8z" fill="#2f7cf6" /><path d="M5.2 5h5.6v1.6H7v1.2h3.2v1.6H7V12H5.2z" fill="#fff" /></svg><b>Forge</b> Studio</div>
      <DocTabs studio={studio} home />
      <div className="spacer" />
      <button className="titlebar-btn" onClick={openSettings} title={t('Settings & Language')}><Icon name="layout" size={15} /></button>
    </div>
  );
}

export function App({ studio }: { studio: Studio }) {
  const [layout, setLayoutState] = useState<Layout>(loadLayout);
  const [dialog, setDialog] = useState<string | null>(null);
  useSignals([studio.changed]);
  const setLayout = (l: Layout) => { setLayoutState(l); saveLayout(l); };
  const layoutRef = useRef(layout);
  layoutRef.current = layout;
  const togglePanel = (id: string) => setLayout(layout.hidden.includes(id) ? showPanel(layout, id) : hidePanel(layout, id));

  useEffect(() => studio.panelRequest.connect((p) => {
    if (['toolbox', 'explorer', 'properties', 'output', 'assets', 'terrain', 'analysis'].includes(p)) setLayout(showPanel(layoutRef.current, p));
    if (p.startsWith('dialog:')) setDialog(p.slice(7));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }), [studio]);
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && e.shiftKey && e.key.toLowerCase() === 'x') { e.preventDefault(); setLayout(showPanel(layoutRef.current, 'explorer')); setTimeout(() => (document.querySelector('[data-explorer-search]') as HTMLInputElement | null)?.focus(), 50); return; }
      if (dialog) return;
      studio.handleKey(e);
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [studio, dialog]);

  const dialogs = (
    <>
      <ContextMenuHost studio={studio} />
      <DialogHost studio={studio} />
      {dialog === 'import-place' && <ImportPlaceDialog studio={studio} onClose={() => setDialog(null)} />}
      {dialog === 'insert' &&<InsertObjectDialog studio={studio} onClose={() => setDialog(null)} />}
      {dialog === 'settings' && <SettingsDialog studio={studio} onClose={() => setDialog(null)} />}
      {dialog === 'shortcuts' && <ShortcutsDialog onClose={() => setDialog(null)} />}
      {dialog === 'about' && <AboutDialog onClose={() => setDialog(null)} />}
      {dialog === 'api' && <ApiDialog onClose={() => setDialog(null)} />}
      {dialog === 'publish' && <PublishDialog studio={studio} onClose={() => setDialog(null)} />}
      {dialog === 'versions' && <VersionsDialog studio={studio} onClose={() => setDialog(null)} />}
    </>
  );

  if (studio.showStart || !studio.active) {
    return (
      <div className="app home-mode">
        <TitleBar studio={studio} openSettings={() => setDialog('settings')} />
        <Home studio={studio} openSettings={() => setDialog('settings')} />
        {dialogs}
      </div>
    );
  }

  const panels: Record<string, PanelDef> = {
    toolbox: { id: 'toolbox', title: t('Toolbox'), icon: 'plus', render: () => <Toolbox studio={studio} openInsertDialog={() => setDialog('insert')} /> },
    explorer: { id: 'explorer', title: t('Explorer'), icon: 'workspace', render: () => <Explorer studio={studio} /> },
    properties: { id: 'properties', title: t('Properties'), icon: 'layout', render: () => <Properties studio={studio} /> },
    output: { id: 'output', title: t('Output'), icon: 'terminal', render: () => <Output studio={studio} /> },
    assets: { id: 'assets', title: t('Assets'), icon: 'library', render: () => <AssetBrowser studio={studio} /> },
    terrain: { id: 'terrain', title: t('Terrain Editor'), icon: 'mountain', render: () => <TerrainPanel studio={studio} /> },
    analysis: { id: 'analysis', title: t('Script Analysis'), icon: 'script', render: () => <ScriptAnalysis studio={studio} /> },
  };

  const doc = studio.active;
  const playing = !!studio.play;
  const ed = !doc || playing;
  const hasSel = !!doc?.selection.size;
  const inScript = !!doc && doc.activeTab !== 'scene';
  const vis = (id: string) => !layout.hidden.includes(id);
  const menus: { label: string; items: () => ContextMenuItem[] }[] = [
    { label: t('File'), items: () => [
      { label: t('New Project'), shortcut: 'Ctrl+N', children: TEMPLATES.map((tp) => ({ label: t(tp.name), action: () => studio.newProject(tp.id) })) },
      { label: t('Open…'), shortcut: 'Ctrl+O', action: () => studio.openProjectDialog() },
      { label: t('Open Recent'), disabled: !studio.recentFiles().length, children: studio.recentFiles().map((r) => ({ label: r.name, action: () => studio.openPath(r.path) })) },
      { label: t('Example Projects'), children: EXAMPLES.map((x) => ({ label: t(x.name), action: () => studio.openExample(x.id) })) },
      { label: t('Start Page'), action: () => studio.showStartPage() },
      { separator: true },
      { label: t('Save'), shortcut: 'Ctrl+S', disabled: !doc, action: () => studio.save() },
      { label: t('Save to File…'), shortcut: 'Ctrl+Shift+S', disabled: !doc, action: () => studio.save(doc, true) },
      { label: t('Autosave Now'), disabled: !doc, action: async () => { const n = await studio.runAutosave(); if (!n) studio.output.system(t('Autosave: nothing to save (no unsaved changes).')); } },
      { separator: true },
      { label: 'Import Place (OpenStreetMap)…', action: () => setDialog('import-place') },
      { label: 'Map (Git)', children: [
        { label: 'Save Map (worldridr-map)', shortcut: 'Ctrl+Alt+S', disabled: !doc || playing, action: () => saveMap(studio) },
        { label: 'Save Map to Other Folder…', disabled: !doc || playing, action: () => saveMap(studio, true) },
        { label: 'Open Map Folder…', action: () => openMapFolder(studio) },
        { separator: true },
        { label: 'Clone Map Repository…', action: () => cloneMap(studio) },
        { label: 'Pull Latest Changes', disabled: !doc, action: () => pullMap(studio) },
        { label: 'Commit & Push…', disabled: !doc || playing, action: () => commitAndPush(studio) },
        { separator: true },
        { label: 'Include imported OpenStreetMap objects', checked: mapOptions.includeOsm, action: () => { mapOptions.includeOsm = !mapOptions.includeOsm; studio.emit(); } },
        { label: 'Include terrain (terrain.json)', checked: mapOptions.terrain, action: () => { mapOptions.terrain = !mapOptions.terrain; studio.emit(); } },
        { label: 'Include scripts', checked: mapOptions.scripts, action: () => { mapOptions.scripts = !mapOptions.scripts; studio.emit(); } },
      ] },
      { separator: true },
      { label: t('Import'), disabled: !doc, children: [
        { label: t('Image…'), disabled: ed, action: () => studio.importMedia('image') },
        { label: t('Sound…'), disabled: ed, action: () => studio.importMedia('sound') },
        { label: t('3D Model (.glb)…'), disabled: ed, action: () => studio.importMedia('glb') },
        { label: t('Assets (any supported file)…'), action: () => studio.importAssets() },
        { label: t('Model File (.forgemodel)…'), disabled: ed, action: () => studio.importModelFile() },
        { label: t('Lua Script…'), disabled: ed, action: () => studio.importLuaScript() },
        { label: t('Scene (.forgescene) as new tab…'), action: () => studio.openProjectDialog() },
      ] },
      { label: t('Export'), disabled: !doc, children: [
        { label: t('Scene (.forgescene)…'), action: () => studio.exportScene() },
        { label: t('Selection as Model (.forgemodel)…'), disabled: !hasSel, action: () => studio.exportSelection() },
        { label: t('Scene/Selection as glTF Binary (.glb)…'), action: () => studio.exportGLB() },
        { label: t('Current Script (.lua)…'), action: () => studio.exportLuaScript() },
      ] },
      { separator: true },
      { label: t('Settings…'), action: () => setDialog('settings') },
      { label: t('Close Project'), disabled: !doc, action: () => doc && studio.closeDoc(doc) },
      ...(isElectron ? [{ separator: true }, { label: t('Exit'), action: async () => { if (await studio.requestQuit()) native!.confirmClose(); } }] : []),
    ] },
    { label: t('Edit'), items: () => [
      { label: `${t('Undo')} ${doc?.history.undoLabel ?? ''}`, shortcut: 'Ctrl+Z', disabled: ed || !doc?.history.canUndo, action: () => studio.undo() },
      { label: `${t('Redo')} ${doc?.history.redoLabel ?? ''}`, shortcut: 'Ctrl+Y', disabled: ed || !doc?.history.canRedo, action: () => studio.redo() },
      { separator: true },
      { label: t('Cut'), shortcut: 'Ctrl+X', disabled: ed || !hasSel, action: () => studio.cut() },
      { label: t('Copy'), shortcut: 'Ctrl+C', disabled: !hasSel, action: () => studio.copy() },
      { label: t('Paste'), shortcut: 'Ctrl+V', disabled: ed || !studio.clipboard, action: () => studio.paste() },
      { label: t('Paste Into'), shortcut: 'Ctrl+Shift+V', disabled: ed || !studio.clipboard || !hasSel, action: () => studio.paste(true) },
      { label: t('Duplicate'), shortcut: 'Ctrl+D', disabled: ed || !hasSel, action: () => studio.duplicate() },
      { label: t('Delete'), shortcut: 'Del', disabled: ed || !hasSel, action: () => studio.deleteSelection() },
      { label: t('Rename'), shortcut: 'F2', disabled: ed || !hasSel, action: () => studio.rename() },
      { separator: true },
      { label: t('Select All'), shortcut: 'Ctrl+A', disabled: !doc, action: () => studio.selectAll() },
      { label: t('Select Children'), disabled: !hasSel, action: () => studio.selectChildren() },
      { label: t('Find in Explorer'), shortcut: 'Ctrl+Shift+X', action: () => { setLayout(showPanel(layout, 'explorer')); setTimeout(() => (document.querySelector('[data-explorer-search]') as HTMLInputElement | null)?.focus(), 50); } },
    ] },
    { label: t('View'), items: () => [
      ...Object.values(panels).map((p) => ({ label: p.title, checked: vis(p.id), action: () => togglePanel(p.id) })),
      { separator: true },
      { label: t('Grid'), checked: studio.showGrid, action: () => studio.toggleGrid() },
      { label: t('Stats'), checked: studio.showStats, action: () => { studio.showStats = !studio.showStats; studio.emit(); } },
      { label: t('Focus Selection'), shortcut: 'F', disabled: !hasSel, action: () => studio.focusSelection() },
      { label: t('UI Editor'), checked: studio.uiEditor, action: () => studio.setUiEditor(!studio.uiEditor) },
      { separator: true },
      { label: t('Reset Layout'), action: () => setLayout(structuredClone(DEFAULT_LAYOUT)) },
    ] },
    { label: t('Insert'), items: () => [
      { label: t('Part'), disabled: ed, children: (['Block', 'Ball', 'Cylinder', 'Wedge'] as const).map((s) => ({ label: t(s), action: () => studio.insertShape(s) })) },
      { label: t('Image…'), disabled: ed, action: () => studio.importMedia('image') },
      { label: t('Sound…'), disabled: ed, action: () => studio.importMedia('sound') },
      { label: t('3D Model (.glb)…'), disabled: ed, action: () => studio.importMedia('glb') },
      { separator: true },
      ...insertMenuItems(studio).map((g) => ({ ...g, disabled: ed })),
      { label: t('Prefabs'), disabled: ed, children: PREFAB_NAMES.map((n) => ({ label: t(n), action: () => studio.insertPrefab(n) })) },
      { separator: true },
      { label: t('Toolbox'), action: () => setLayout(showPanel(layout, 'toolbox')) },
      { label: t('Insert Object…'), disabled: ed, action: () => setDialog('insert') },
      { label: t('Model from File…'), disabled: ed, action: () => studio.importModelFile() },
    ] },
    { label: t('Model'), items: () => [
      { label: t('Select'), shortcut: 'Ctrl+1', checked: studio.tool === 'select', disabled: ed, action: () => studio.setTool('select') },
      { label: t('Move'), shortcut: 'Ctrl+2', checked: studio.tool === 'move', disabled: ed, action: () => studio.setTool('move') },
      { label: t('Scale'), shortcut: 'Ctrl+3', checked: studio.tool === 'scale', disabled: ed, action: () => studio.setTool('scale') },
      { label: t('Rotate'), shortcut: 'Ctrl+4', checked: studio.tool === 'rotate', disabled: ed, action: () => studio.setTool('rotate') },
      { label: t('Local Space'), shortcut: 'Ctrl+L', checked: studio.viewport?.space === 'local', action: () => { studio.viewport?.setSpace(studio.viewport.space === 'world' ? 'local' : 'world'); studio.emit(); } },
      { label: t('Snap to Grid'), checked: !!doc?.settings.snapEnabled, disabled: !doc, action: () => { if (doc) { doc.settings.snapEnabled = !doc.settings.snapEnabled; doc.history.touch(); studio.viewport?.updateGizmo(); } } },
      { separator: true },
      { label: t('Group as Model'), shortcut: 'Ctrl+G', disabled: ed || !hasSel, action: () => studio.group('Model') },
      { label: t('Group as Folder'), shortcut: 'Ctrl+Alt+G', disabled: ed || !hasSel, action: () => studio.group('Folder') },
      { label: t('Ungroup'), shortcut: 'Ctrl+U', disabled: ed || !hasSel, action: () => studio.ungroup() },
      { separator: true },
      { label: t('Toggle Anchored'), disabled: ed || !hasSel, action: () => studio.toggleProp('Anchored') },
      { label: t('Toggle Can Collide'), disabled: ed || !hasSel, action: () => studio.toggleProp('CanCollide') },
      { label: t('Toggle Visible'), disabled: ed || !hasSel, action: () => studio.toggleProp('Visible') },
      { label: t('Toggle Locked'), disabled: ed || !hasSel, action: () => studio.toggleProp('Locked') },
      { separator: true },
      { label: t('Save Selection to Asset Library'), disabled: ed || !hasSel, action: () => studio.saveSelectionAsAsset() },
    ] },
    { label: t('Test'), items: () => [
      { label: playing ? (studio.play!.paused ? t('Resume') : t('Pause')) : t('Play'), shortcut: 'F5', disabled: !doc, action: () => (playing ? studio.togglePause() : studio.startPlay()) },
      { label: t('Stop'), shortcut: 'Shift+F5', disabled: !playing, action: () => studio.stopPlay() },
      { label: t('Restart'), disabled: !playing, action: () => studio.restartPlay() },
      { separator: true },
      ...(isElectron ? [
        { label: studio.localServer?.running ? t('Stop Local Server') : t('Start Local Server'), action: () => studio.toggleLocalServer() },
        { label: t('Open Forge Player'), action: () => native!.launchPlayer() },
        { separator: true },
      ] : []),
      { label: t('Run Self-Test'), disabled: playing, action: () => studio.runSelfTest() },
    ] },
    { label: t('Terrain'), items: () => [
      { label: t('Terrain Editor'), checked: studio.tool === 'terrain', disabled: ed, action: () => studio.setTool(studio.tool === 'terrain' ? 'select' : 'terrain') },
      { label: t('Generate…'), disabled: ed, action: () => setLayout(showPanel(layout, 'terrain')) },
    ] },
    { label: t('Script'), items: () => [
      { label: t('New Script'), disabled: ed, action: () => studio.insertObject('Script') },
      { label: t('New LocalScript'), disabled: ed, action: () => studio.insertObject('LocalScript') },
      { label: t('New ModuleScript'), disabled: ed, action: () => studio.insertObject('ModuleScript') },
      { separator: true },
      { label: t('Find'), shortcut: 'Ctrl+F', disabled: !inScript, action: () => runEditorAction('find') },
      { label: t('Replace'), shortcut: 'Ctrl+H', disabled: !inScript, action: () => runEditorAction('replace') },
      { label: t('Format Document'), shortcut: 'Shift+Alt+F', disabled: !inScript, action: () => runEditorAction('format') },
      { separator: true },
      { label: t('Import Lua File…'), disabled: ed, action: () => studio.importLuaScript() },
      { label: t('Save Script to File…'), disabled: !doc, action: () => studio.exportLuaScript() },
      { label: t('Script Analysis'), checked: vis('analysis'), action: () => togglePanel('analysis') },
    ] },
    { label: t('Help'), items: () => [
      { label: t('Keyboard & Mouse'), action: () => setDialog('shortcuts') },
      { label: t('Scripting API'), action: () => setDialog('api') },
      { label: t('About Forge Studio'), action: () => setDialog('about') },
    ] },
  ];

  return (
    <div className="app">
      <MenuBar menus={menus} />
      <Ribbon studio={studio} layout={layout} togglePanel={togglePanel} openDialog={(d) => (d === 'resetLayout' ? setLayout(structuredClone(DEFAULT_LAYOUT)) : setDialog(d))} />
      <DocTabs studio={studio} />
      <DockLayout layout={layout} setLayout={setLayout} panels={panels} center={<Center studio={studio} />} />
      <StatusBar studio={studio} />
      {dialogs}
    </div>
  );
}

function ApiDialog({ onClose }: { onClose: () => void }) {
  return <Modal title={t('Scripting API')} onClose={onClose} width={820}><div className="modal-body"><pre className="api-doc">{apiDoc}</pre></div></Modal>;
}

/** Re-mounts the UI when the language changes so every string is re-translated. */
export function Root({ studio }: { studio: Studio }) {
  const [lang, setLang] = useState(0);
  useEffect(() => languageChanged.connect(() => setLang((x) => x + 1)), []);
  useEffect(() => {
    native?.onCloseRequested(async () => { if (await studio.requestQuit()) native!.confirmClose(); });
    const before = (e: BeforeUnloadEvent) => { if (!isElectron && studio.docs.some((d) => d.dirty)) { e.preventDefault(); e.returnValue = ''; } };
    window.addEventListener('beforeunload', before);
    return () => window.removeEventListener('beforeunload', before);
  }, [studio]);
  return <App key={lang} studio={studio} />;
}

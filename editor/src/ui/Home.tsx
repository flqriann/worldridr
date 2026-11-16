// Start screen: clean, panel-free home with templates, the project library
// ("My Projects") and examples – like the Roblox Studio start page.

import { useEffect, useState } from 'react';
import type { Studio } from '../editor/Studio';
import { EXAMPLES, TEMPLATES } from '../project/templates';
import { library, type LibraryEntry } from '../project/library';
import { isElectron, native } from '../project/ProjectIO';
import { currentLanguage, t } from '../i18n';
import { Icon } from './icons';
import { useSignals } from './hooks';

const ART: Record<string, string> = {
  baseplate: 'linear-gradient(160deg,#4a8de0 0%,#bfe0ff 55%,#6b6f76 56%,#55585e 100%)',
  'flat-terrain': 'linear-gradient(160deg,#4a8de0 0%,#bfe0ff 55%,#5a8f3a 56%,#3f6b28 100%)',
  empty: 'linear-gradient(160deg,#2a2d33,#1b1c1f)',
  obby: 'linear-gradient(160deg,#ff9f1a,#ffd43b 40%,#4dabf7 41%,#9775fa)',
  physics: 'linear-gradient(160deg,#4dabf7,#b07a40 60%,#ff6b6b)',
  island: 'linear-gradient(160deg,#8ec8ff 0%,#c4e1ff 45%,#5a8f3a 46%,#d8c48a 70%,#1f6aa5 71%)',
};

type Page = 'home' | 'projects' | 'templates';

function when(ms: number) {
  const d = new Date(ms);
  const diff = Date.now() - ms;
  const loc = currentLanguage();
  if (diff < 60_000) return t('just now');
  if (diff < 3_600_000) return t('{n} min ago', { n: Math.floor(diff / 60_000) });
  if (diff < 86_400_000) return t('{n} h ago', { n: Math.floor(diff / 3_600_000) });
  return new Intl.DateTimeFormat(loc, { dateStyle: 'medium' }).format(d);
}

function ProjectCard({ e, studio, reload }: { e: LibraryEntry; studio: Studio; reload: () => void }) {
  const menu = (ev: React.MouseEvent) => {
    ev.preventDefault();
    studio.contextMenu.fire({ x: ev.clientX, y: ev.clientY, items: [
      { label: t('Open'), action: () => studio.openLibraryProject(e.key, e.name) },
      { label: t('Rename…'), action: async () => { const n = await studio.prompt(t('Rename project'), t('New name:'), e.name); if (n?.trim()) { await library.rename(e.key, n.trim()); reload(); } } },
      ...(isElectron ? [{ label: t('Show in folder'), action: () => library.reveal(e.key) }] : []),
      { separator: true },
      { label: isElectron ? t('Move to recycle bin') : t('Delete'), action: async () => {
        const r = await studio.ask(t('Delete project'), isElectron ? t('Move "{name}" to the recycle bin?', { name: e.name }) : t('Delete "{name}" permanently from this browser?', { name: e.name }), [{ id: 'del', label: t('Delete'), danger: true }, { id: 'cancel', label: t('Cancel') }]);
        if (r.button === 'del') { await library.remove(e.key); reload(); }
      } },
    ] });
  };
  return (
    <button className="home-card project" onClick={() => studio.openLibraryProject(e.key, e.name)} onContextMenu={menu} title={e.key}>
      <div className="home-card-art" style={e.thumbnail ? { backgroundImage: `url(${e.thumbnail})` } : undefined}>{!e.thumbnail && <Icon name="game" size={34} />}</div>
      <div className="home-card-body">
        <div className="home-card-title">{e.name}</div>
        <div className="home-card-meta">{when(e.modified)} · {(e.size / 1024).toFixed(0)} KB</div>
      </div>
      <span className="home-card-more" onClick={(ev) => { ev.stopPropagation(); menu(ev); }}><Icon name="dots" size={14} /></span>
    </button>
  );
}

function TemplateCard({ id, name, description, onClick }: { id: string; name: string; description: string; onClick: () => void }) {
  return (
    <button className="home-card" onClick={onClick}>
      <div className="home-card-art" style={{ background: ART[id] }} />
      <div className="home-card-body">
        <div className="home-card-title">{t(name)}</div>
        <div className="home-card-meta">{t(description)}</div>
      </div>
    </button>
  );
}

export function Home({ studio, openSettings }: { studio: Studio; openSettings: () => void }) {
  useSignals([studio.changed]);
  const [page, setPage] = useState<Page>('home');
  const [entries, setEntries] = useState<LibraryEntry[] | null>(null);
  const [query, setQuery] = useState('');
  const reload = () => { library.list().then(setEntries).catch(() => setEntries([])); };
  useEffect(reload, [studio.docs.length]);

  const filtered = (entries ?? []).filter((e) => !query || e.name.toLowerCase().includes(query.toLowerCase()));
  const nav = (id: Page, icon: string, label: string) => (
    <button className={'home-nav-item' + (page === id ? ' active' : '')} onClick={() => setPage(id)}><Icon name={icon} size={16} />{label}</button>
  );

  return (
    <div className="home">
      <nav className="home-nav">
        {nav('home', 'layout', t('Home'))}
        {nav('projects', 'save', t('My Projects'))}
        {nav('templates', 'library', t('Templates'))}
        <div className="home-nav-sep" />
        <button className="home-nav-item" onClick={() => studio.openProjectDialog()}><Icon name="open" size={16} />{t('Open from file…')}</button>
        {isElectron && <button className="home-nav-item" onClick={() => native!.launchPlayer()}><Icon name="play" size={16} />{t('Open Forge Player')}</button>}
        <div className="spacer" />
        <button className="home-nav-item" onClick={openSettings}><Icon name="layout" size={16} />{t('Settings & Language')}</button>
      </nav>
      <main className="home-main">
        {page === 'home' && <>
          <header className="home-hero">
            <h1>{t('Create something new')}</h1>
            <p>{t('Start from a template, continue a project or explore an example game.')}</p>
          </header>
          <section>
            <h2>{t('New project')}</h2>
            <div className="home-grid">
              {TEMPLATES.map((tp) => <TemplateCard key={tp.id} id={tp.id} name={tp.name} description={tp.description} onClick={() => studio.newProject(tp.id)} />)}
            </div>
          </section>
          <section>
            <div className="home-section-head">
              <h2>{t('Recent projects')}</h2>
              {(entries?.length ?? 0) > 6 && <button className="link" onClick={() => setPage('projects')}>{t('Show all')}</button>}
            </div>
            {entries === null ? <div className="home-empty">{t('Loading…')}</div>
              : entries.length ? <div className="home-grid">{entries.slice(0, 6).map((e) => <ProjectCard key={e.key} e={e} studio={studio} reload={reload} />)}</div>
                : <div className="home-empty">{t('No saved projects yet. Create one above – Ctrl+S saves it to My Projects.')}</div>}
          </section>
        </>}
        {page === 'projects' && <>
          <header className="home-hero compact">
            <h1>{t('My Projects')}</h1>
            <div className="search-box home-search"><Icon name="search" size={14} /><input placeholder={t('Search projects')} value={query} onChange={(e) => setQuery(e.target.value)} /></div>
          </header>
          {filtered.length ? <div className="home-grid">{filtered.map((e) => <ProjectCard key={e.key} e={e} studio={studio} reload={reload} />)}</div>
            : <div className="home-empty">{query ? t('No projects match your search.') : t('No saved projects yet.')}</div>}
          <p className="hint">{isElectron ? t('Projects are stored in Documents/WorldRidr Studio/Projects.') : t('Projects are stored in this browser. Use File > Save to File to keep a copy on disk.')}</p>
        </>}
        {page === 'templates' && <>
          <header className="home-hero compact"><h1>{t('Templates')}</h1></header>
          <section>
            <h2>{t('Start fresh')}</h2>
            <div className="home-grid">{TEMPLATES.map((tp) => <TemplateCard key={tp.id} id={tp.id} name={tp.name} description={tp.description} onClick={() => studio.newProject(tp.id)} />)}</div>
          </section>
          <section>
            <h2>{t('Example games')}</h2>
            <div className="home-grid">{EXAMPLES.map((x) => <TemplateCard key={x.id} id={x.id} name={x.name} description={x.description} onClick={() => studio.openExample(x.id)} />)}</div>
          </section>
        </>}
      </main>
    </div>
  );
}

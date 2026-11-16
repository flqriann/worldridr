// Toolbox: the place to insert things. Clear categories, a description and
// destination on every card, search (English/German keywords), recently used
// items, click to insert at the view center or drag onto the 3D view.

import { useEffect, useState } from 'react';
import type { Studio } from '../../editor/Studio';
import { TOOLBOX_CATEGORIES, recentToolbox, searchToolbox, type ToolboxCategory, type ToolboxItem } from '../../editor/toolbox';
import { getBuiltinAsset } from '../../assets/builtin';
import { modelThumbnail } from '../../render/thumbnails';
import { Icon } from '../icons';
import { useSignals } from '../hooks';
import { t } from '../../i18n';

function Thumb({ item }: { item: ToolboxItem }) {
  const [url, setUrl] = useState<string | null>(null);
  useEffect(() => {
    let alive = true;
    const a = item.thumbAsset ? getBuiltinAsset(item.thumbAsset) : undefined;
    if (a) modelThumbnail(a).then((u) => alive && setUrl(u)).catch(() => {});
    return () => { alive = false; };
  }, [item]);
  if (url) return <div className="tb-thumb" style={{ backgroundImage: `url(${url})` }} />;
  return <div className="tb-thumb icon"><Icon name={item.icon} size={26} /></div>;
}

function Card({ item, studio, compact }: { item: ToolboxItem; studio: Studio; compact?: boolean }) {
  const sel = studio.active?.selection.primary;
  const hasPart = !!studio.selectedPart() || sel?.className === 'Model';
  const blocked = item.needsPart && !hasPart;
  return (
    <button className={'tb-card' + (compact ? ' compact' : '') + (blocked ? ' needs' : '')} draggable
      title={`${t(item.name)}\n${t(item.description)}\n\n${t('Inserts into: {target}', { target: t(item.target) })}${blocked ? '\n\n' + t('Select a part first.') : ''}\n${t('Click to insert, or drag into the 3D view.')}`}
      onDragStart={(e) => { e.dataTransfer.setData('text/forge-toolbox', item.id); e.dataTransfer.effectAllowed = 'copy'; }}
      onClick={() => studio.useToolboxItem(item)}>
      <Thumb item={item} />
      <div className="tb-text">
        <div className="tb-name">{t(item.name)}</div>
        {!compact && <div className="tb-desc">{t(item.description)}</div>}
        {!compact && <div className={'tb-target' + (blocked ? ' warn' : '')}><Icon name={blocked ? 'warn' : 'chevronRight'} size={10} />{blocked ? t('Select a part first') : t(item.target)}</div>}
      </div>
    </button>
  );
}

export function Toolbox({ studio, openInsertDialog }: { studio: Studio; openInsertDialog: () => void }) {
  useSignals([studio.changed, studio.active?.selection.changed]);
  const [query, setQuery] = useState('');
  const [cat, setCat] = useState<ToolboxCategory | 'all'>('all');
  const disabled = !studio.editDoc;
  const results = searchToolbox(query, cat);
  const recent = !query && cat === 'all' ? recentToolbox().slice(0, 6) : [];

  const sections = query || cat !== 'all'
    ? [{ id: 'results', label: query ? t('{n} results', { n: results.length }) : t(TOOLBOX_CATEGORIES.find((c) => c.id === cat)!.hint), items: results }]
    : TOOLBOX_CATEGORIES.map((c) => ({ id: c.id, label: t(c.label), items: results.filter((i) => i.category === c.id) }));

  return (
    <div className={'toolbox' + (disabled ? ' disabled' : '')}>
      <div className="panel-toolbar">
        <div className="search-box">
          <Icon name="search" size={13} />
          <input placeholder={t('Search: light, coin, button, tree, fire…')} value={query} onChange={(e) => setQuery(e.target.value)} />
          {query && <button className="icon-btn" onClick={() => setQuery('')}><Icon name="close" size={11} /></button>}
        </div>
      </div>
      <div className="tb-cats">
        <button className={cat === 'all' ? 'on' : ''} onClick={() => setCat('all')}>{t('All')}</button>
        {TOOLBOX_CATEGORIES.map((c) => (
          <button key={c.id} className={cat === c.id ? 'on' : ''} onClick={() => setCat(c.id)} title={t(c.hint)}><Icon name={c.icon} size={12} />{t(c.label)}</button>
        ))}
      </div>
      {disabled && <div className="tb-banner">{studio.play ? t('Stop the test to insert objects.') : t('Open or create a project to insert objects.')}</div>}
      <div className="tb-scroll">
        {recent.length > 0 && (
          <div className="tb-section">
            <div className="tb-section-title">{t('Recently used')}</div>
            <div className="tb-recent">{recent.map((i) => <Card key={i.id} item={i} studio={studio} compact />)}</div>
          </div>
        )}
        {sections.filter((s) => s.items.length).map((s) => (
          <div key={s.id} className="tb-section">
            <div className="tb-section-title">{s.label}</div>
            <div className="tb-grid">{s.items.map((i) => <Card key={i.id} item={i} studio={studio} />)}</div>
          </div>
        ))}
        {!results.length && <div className="panel-empty">{t('Nothing found for "{q}". Try another word, or browse all object classes.', { q: query })}</div>}
        <button className="tb-more" onClick={openInsertDialog} disabled={disabled}><Icon name="plus" size={12} /> {t('All object classes…')}</button>
        <p className="hint tb-hint">{t('Tip: drag a card – or an image, sound or .glb file from your computer – into the 3D view to drop it exactly where you want. Materials, textures and imported files are in the Assets panel.')}</p>
      </div>
    </div>
  );
}

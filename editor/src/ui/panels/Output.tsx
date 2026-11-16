// Output console with level filters, clickable script errors and a Lua
// command bar (runs in the running test, or against the edit scene with undo).

import { useEffect, useRef, useState } from 'react';
import type { Studio } from '../../editor/Studio';
import type { LogEntry, LogLevel } from '../../editor/Output';
import { Icon } from '../icons';
import { useSignals } from '../hooks';

import { t } from '../../i18n';
const LEVEL_ICON: Record<LogLevel, string> = { info: 'info', warn: 'warn', error: 'error', print: 'terminal', system: 'info' };

export function Output({ studio }: { studio: Studio }) {
  const log = studio.output;
  useSignals([log.changed, studio.changed]);
  const [filter, setFilter] = useState<Record<LogLevel, boolean>>({ info: true, warn: true, error: true, print: true, system: true });
  const [query, setQuery] = useState('');
  const [cmd, setCmd] = useState('');
  const history = useRef<string[]>([]);
  const histIdx = useRef(-1);
  const scrollRef = useRef<HTMLDivElement>(null);
  const stick = useRef(true);

  const entries = log.entries.filter((e) => filter[e.level] && (!query || e.message.toLowerCase().includes(query.toLowerCase())));
  useEffect(() => { const el = scrollRef.current; if (el && stick.current) el.scrollTop = el.scrollHeight; });

  const openSource = (e: LogEntry) => {
    if (!e.source) return;
    const doc = studio.active;
    const inst = doc?.dm.byId(e.source.scriptId);
    if (!inst) { studio.output.warn(t('The script for this message no longer exists in the edit scene.')); return; }
    studio.openScript(inst);
    if (e.source.line) studio.panelRequest.fire(`goto:${inst.id}:${e.source.line}`);
  };

  const run = () => {
    const code = cmd.trim();
    if (!code) return;
    history.current.unshift(code);
    histIdx.current = -1;
    setCmd('');
    studio.runCommand(code);
  };

  const toggle = (l: LogLevel) => setFilter({ ...filter, [l]: !filter[l] });
  return (
    <div className="output">
      <div className="panel-toolbar">
        <button className={'chip' + (filter.print && filter.info && filter.system ? ' on' : '')} onClick={() => { const v = !filter.print; setFilter({ ...filter, print: v, info: v, system: v }); }}><Icon name="info" size={12} /> {t('Messages')}</button>
        <button className={'chip' + (filter.warn ? ' on' : '')} onClick={() => toggle('warn')}><Icon name="warn" size={12} color="#f5c04a" /> {t('Warnings')} {log.warnCount || ''}</button>
        <button className={'chip' + (filter.error ? ' on' : '')} onClick={() => toggle('error')}><Icon name="error" size={12} color="#ff6b6b" /> {t('Errors')} {log.errorCount || ''}</button>
        <div className="search-box small"><Icon name="search" size={12} /><input placeholder={t('Filter')} value={query} onChange={(e) => setQuery(e.target.value)} /></div>
        <div className="spacer" />
        <button className="btn small" onClick={() => log.clear()} title={t('Clear output')}><Icon name="trash" size={12} /> {t('Clear')}</button>
      </div>
      <div className="log" ref={scrollRef} onScroll={(e) => { const el = e.target as HTMLElement; stick.current = el.scrollTop + el.clientHeight >= el.scrollHeight - 8; }}>
        {entries.map((e) => (
          <div key={e.id} className={`log-line log-${e.level}` + (e.source ? ' clickable' : '')} onClick={() => openSource(e)} title={e.source ? 'Click to open the script' : ''}>
            <span className="log-time">{new Date(e.time).toLocaleTimeString()}</span>
            <Icon name={LEVEL_ICON[e.level]} size={12} />
            {e.source?.name && <span className="log-src">{e.source.name}{e.source.line ? `:${e.source.line}` : ''}</span>}
            <span className="log-msg">{e.message}</span>
            {e.count > 1 && <span className="log-count">×{e.count}</span>}
          </div>
        ))}
        {!entries.length && <div className="panel-empty">{t('Output is empty.')}</div>}
      </div>
      <div className="command-bar">
        <span className="cmd-prompt">{studio.play ? t('Game') : t('Edit')} ›</span>
        <input placeholder={studio.play ? t('Run Lua in the running game, e.g. workspace.Gravity = 50') : t('Run Lua on the edit scene (undoable), e.g. for i=1,5 do Instance.new("Part", workspace) end')}
          value={cmd} onChange={(e) => setCmd(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Enter') run();
            if (e.key === 'ArrowUp') { histIdx.current = Math.min(history.current.length - 1, histIdx.current + 1); setCmd(history.current[histIdx.current] ?? cmd); }
            if (e.key === 'ArrowDown') { histIdx.current = Math.max(-1, histIdx.current - 1); setCmd(histIdx.current < 0 ? '' : history.current[histIdx.current]); }
          }} />
      </div>
    </div>
  );
}

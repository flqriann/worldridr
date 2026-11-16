// Script Analysis: syntax-checks every script in the project (Lua compiler).

import { useEffect, useState } from 'react';
import type { Studio } from '../../editor/Studio';
import type { Instance } from '../../core/Instance';
import { LuaRuntime } from '../../runtime/LuaRuntime';
import { ClassIcon, Icon } from '../icons';
import { useSignals } from '../hooks';

import { t } from '../../i18n';
interface Problem { inst: Instance; line: number; message: string }

export function ScriptAnalysis({ studio }: { studio: Studio }) {
  useSignals([studio.changed]);
  const doc = studio.active;
  const [problems, setProblems] = useState<Problem[]>([]);
  const [count, setCount] = useState(0);
  const [version, setVersion] = useState(0);

  useEffect(() => {
    if (!doc) return;
    let t: number | undefined;
    const off = doc.dm.changed.connect((_i, p) => { if (p === 'Source') { clearTimeout(t); t = window.setTimeout(() => setVersion((v) => v + 1), 600); } });
    const off2 = doc.dm.added.connect((i) => { if (i.isA('LuaSourceContainer')) setVersion((v) => v + 1); });
    return () => { off(); off2(); clearTimeout(t); };
  }, [doc]);

  useEffect(() => {
    if (!doc) return;
    let alive = true;
    (async () => {
      const scripts = doc.dm.getDescendants().filter((i) => i.isA('LuaSourceContainer'));
      const out: Problem[] = [];
      for (const s of scripts) {
        const r = await LuaRuntime.checkSyntax(s.props.Source as string);
        if (r) out.push({ inst: s, line: r.line, message: r.message });
      }
      if (alive) { setProblems(out); setCount(scripts.length); }
    })().catch((e) => studio.output.error(t('Script analysis failed: {error}', { error: (e as Error).message })));
    return () => { alive = false; };
  }, [doc, version]);

  if (!doc) return <div className="panel-empty">{t('No project open')}</div>;
  return (
    <div className="analysis">
      <div className="panel-toolbar">
        <span className="dim">{t('{n} scripts checked', { n: count })} · {problems.length ? t('{n} with errors', { n: problems.length }) : t('no syntax errors')}</span>
        <div className="spacer" />
        <button className="btn small" onClick={() => setVersion((v) => v + 1)}><Icon name="restart" size={12} /> {t('Re-check')}</button>
      </div>
      <div className="log">
        {problems.map((p, k) => (
          <div key={k} className="log-line log-error clickable" onClick={() => { studio.openScript(p.inst); studio.panelRequest.fire(`goto:${p.inst.id}:${p.line}`); }}>
            <ClassIcon className={p.inst.className} />
            <span className="log-src">{p.inst.getFullName()}:{p.line}</span>
            <span className="log-msg">{p.message}</span>
          </div>
        ))}
      </div>
    </div>
  );
}

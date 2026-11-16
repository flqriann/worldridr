// Publish to a Forge Server and manage published versions (git-like history,
// live version, rollback, checkout).

import { useEffect, useState } from 'react';
import type { Studio } from '../editor/Studio';
import { ApiError, ServerApi, normalizeServerUrl, type GameDetails, type GameInfo } from '../net/api';
import { isElectron } from '../project/ProjectIO';
import { currentLanguage, t } from '../i18n';
import { Modal } from './Dialogs';
import { Icon } from './icons';

const SERVER_KEY = 'forge.lastServer';
const AUTHOR_KEY = 'forge.author';
const store = { get: (k: string, d = '') => { try { return localStorage.getItem(k) ?? d; } catch { return d; } }, set: (k: string, v: string) => { try { localStorage.setItem(k, v); } catch { /* ignore */ } } };
const fmtDate = (ms: number) => new Intl.DateTimeFormat(currentLanguage(), { dateStyle: 'medium', timeStyle: 'short' }).format(new Date(ms));

function ServerField({ studio, server, setServer, keyValue, setKey, showKey = true }: { studio: Studio; server: string; setServer: (s: string) => void; keyValue: string; setKey: (k: string) => void; showKey?: boolean }) {
  const [busy, setBusy] = useState(false);
  const startLocal = async () => {
    setBusy(true);
    const info = studio.localServer?.running ? studio.localServer : await studio.toggleLocalServer();
    setBusy(false);
    if (info?.url) { setServer(info.url); if (info.publishKey) setKey(info.publishKey); }
  };
  return (
    <>
      <label className="field-col">{t('Server address')}
        <div className="row">
          <input className="text-input" value={server} placeholder="http://127.0.0.1:8787" onChange={(e) => { setServer(e.target.value); setKey(studio.publishKeyFor(e.target.value)); }} />
          {isElectron && <button className="btn small" disabled={busy} onClick={startLocal}><Icon name="play" size={11} /> {studio.localServer?.running ? t('Use local server') : t('Start local server')}</button>}
        </div>
      </label>
      {showKey && <label className="field-col">{t('Publish key')}
        <input className="text-input" type="password" value={keyValue} placeholder={t('printed by forge-server at startup')} onChange={(e) => setKey(e.target.value)} />
      </label>}
    </>
  );
}

export function PublishDialog({ studio, onClose }: { studio: Studio; onClose: () => void }) {
  const doc = studio.active!;
  const target = doc.settings.publish;
  const [server, setServer] = useState(target?.server ?? store.get(SERVER_KEY, studio.localServer?.url ?? 'http://127.0.0.1:8787'));
  const [key, setKey] = useState(studio.publishKeyFor(target?.server ?? store.get(SERVER_KEY, 'http://127.0.0.1:8787')));
  const [games, setGames] = useState<GameInfo[] | null>(null);
  const [gameId, setGameId] = useState<string>(target?.gameId ?? '');
  const [newName, setNewName] = useState(doc.name);
  const [description, setDescription] = useState(doc.settings.description);
  const [message, setMessage] = useState('');
  const [author, setAuthor] = useState(store.get(AUTHOR_KEY, ''));
  const [status, setStatus] = useState<{ kind: 'info' | 'error' | 'ok' | 'conflict'; text: string } | null>(null);
  const [busy, setBusy] = useState(false);

  useEffect(() => { studio.refreshLocalServer(); }, [studio]);
  useEffect(() => {
    let alive = true;
    setGames(null);
    const id = setTimeout(() => {
      new ServerApi(server).games().then((g) => { if (alive) setGames(g); }).catch((e) => { if (alive) { setGames([]); setStatus({ kind: 'error', text: (e as Error).message }); } });
    }, 300);
    return () => { alive = false; clearTimeout(id); };
  }, [server]);

  const publish = async (force = false) => {
    setBusy(true);
    setStatus({ kind: 'info', text: t('Publishing…') });
    store.set(SERVER_KEY, normalizeServerUrl(server));
    store.set(AUTHOR_KEY, author);
    try {
      const v = await studio.publish({ server, key, gameId: gameId || null, newGame: { name: newName, description }, message: message.trim() || t('Update'), author: author.trim() || t('Anonymous'), force });
      setStatus({ kind: 'ok', text: t('Version {v} is live. Players can join it now in Forge Player.', { v: v.number }) });
      setGameId(doc.settings.publish?.gameId ?? '');
      setMessage('');
    } catch (e) {
      const err = e as ApiError;
      if (err.status === 409) setStatus({ kind: 'conflict', text: t('Someone published a newer version since your last publish ({detail}). Open "Versions & Rollback" to check it, or publish anyway as a new version on top.', { detail: err.message }) });
      else if (err.status === 401) setStatus({ kind: 'error', text: t('The publish key is wrong. The server prints it when it starts (or use the local server).') });
      else setStatus({ kind: 'error', text: err.message });
    } finally { setBusy(false); }
  };

  return (
    <Modal title={t('Publish to Server')} onClose={onClose} width={560}>
      <div className="modal-body form publish">
        <p className="hint">{t('Publishing creates a new, permanent version of your game on the server and makes it live. Everything you change in Studio afterwards is development only – players see it after your next publish.')}</p>
        <ServerField studio={studio} server={server} setServer={setServer} keyValue={key} setKey={setKey} />
        <label className="field-col">{t('Game')}
          <select value={gameId} onChange={(e) => setGameId(e.target.value)}>
            <option value="">{t('+ Create a new game')}</option>
            {(games ?? []).map((g) => <option key={g.id} value={g.id}>{g.name} ({g.id}){g.live ? ` – v${g.live}` : ''}</option>)}
          </select>
        </label>
        {!gameId && <>
          <label className="field-col">{t('Game name')}<input className="text-input" value={newName} onChange={(e) => setNewName(e.target.value)} /></label>
          <label className="field-col">{t('Description')}<input className="text-input" value={description} onChange={(e) => setDescription(e.target.value)} /></label>
        </>}
        {target && gameId === target.gameId && <p className="hint">{t('This project is based on version {v}.', { v: target.version })}</p>}
        <label className="field-col">{t('What changed? (version message)')}
          <input className="text-input" value={message} placeholder={t('e.g. Added a new level')} onChange={(e) => setMessage(e.target.value)} onKeyDown={(e) => { if (e.key === 'Enter' && !busy) publish(); }} />
        </label>
        <label className="field-col">{t('Your name (author)')}<input className="text-input" value={author} onChange={(e) => setAuthor(e.target.value)} /></label>
        {status && <div className={'status-box ' + status.kind}>{status.text}</div>}
      </div>
      <div className="modal-buttons">
        {status?.kind === 'conflict' && <button className="btn danger" disabled={busy} onClick={() => publish(true)}>{t('Publish anyway')}</button>}
        <button className="btn primary" disabled={busy || !key} onClick={() => publish()}><Icon name="exportIcon" size={13} /> {t('Publish')}</button>
        <button className="btn" onClick={onClose}>{t('Close')}</button>
      </div>
    </Modal>
  );
}

export function VersionsDialog({ studio, onClose }: { studio: Studio; onClose: () => void }) {
  const doc = studio.active;
  const target = doc?.settings.publish;
  const [server, setServer] = useState(target?.server ?? store.get(SERVER_KEY, 'http://127.0.0.1:8787'));
  const [key, setKey] = useState(studio.publishKeyFor(server));
  const [gameId, setGameId] = useState(target?.gameId ?? '');
  const [games, setGames] = useState<GameInfo[]>([]);
  const [details, setDetails] = useState<GameDetails | null>(null);
  const [error, setError] = useState<string | null>(null);

  const load = () => {
    setError(null);
    const api = new ServerApi(server);
    api.games().then((g) => { setGames(g); if (!gameId && g[0]) setGameId(g[0].id); }).catch((e) => setError((e as Error).message));
    if (gameId) api.game(gameId).then(setDetails).catch((e) => { setDetails(null); setError((e as Error).message); });
  };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  useEffect(load, [server, gameId]);

  const makeLive = async (n: number) => {
    try {
      await new ServerApi(server, key).setLive(gameId, n);
      studio.output.system(t('Version {v} is now live.', { v: n }));
      load();
    } catch (e) { setError((e as Error).message); }
  };

  return (
    <Modal title={t('Versions & Rollback')} onClose={onClose} width={720}>
      <div className="modal-body form">
        <ServerField studio={studio} server={server} setServer={setServer} keyValue={key} setKey={setKey} />
        <label className="field-col">{t('Game')}
          <select value={gameId} onChange={(e) => setGameId(e.target.value)}>
            {!games.length && <option value="">{t('No games on this server yet')}</option>}
            {games.map((g) => <option key={g.id} value={g.id}>{g.name} ({g.id})</option>)}
          </select>
        </label>
        {error && <div className="status-box error">{error}</div>}
        {details && (
          <div className="version-list">
            {[...details.history].reverse().map((v) => (
              <div key={v.number} className={'version-row' + (details.live === v.number ? ' live' : '')}>
                <div className="version-num">v{v.number}</div>
                <div className="version-main">
                  <div className="version-msg">{v.message || <span className="dim">{t('(no message)')}</span>}</div>
                  <div className="dim">{v.author || t('Anonymous')} · {fmtDate(v.created)} · {(v.size / 1024).toFixed(0)} KB · <code>{v.hash.slice(0, 10)}</code></div>
                </div>
                {details.live === v.number ? <span className="badge live-badge">{t('LIVE')}</span>
                  : <button className="btn small" disabled={!key} onClick={() => makeLive(v.number)} title={t('Players joining from now on get this version')}>{t('Make live')}</button>}
                <button className="btn small" onClick={() => studio.checkoutVersion(server, gameId, v.number).then(onClose).catch((e) => setError((e as Error).message))} title={t('Open this version as a new project tab')}>{t('Open')}</button>
              </div>
            ))}
            {!details.history.length && <div className="dim">{t('Nothing published yet.')}</div>}
          </div>
        )}
        {details && details.sessions.length > 0 && <p className="hint">{t('Running sessions:')} {details.sessions.map((s) => `v${s.version}: ${s.players.map((p) => p.name).join(', ')}`).join(' · ')}</p>}
      </div>
      <div className="modal-buttons"><button className="btn" onClick={load}><Icon name="restart" size={12} /> {t('Refresh')}</button><button className="btn" onClick={onClose}>{t('Close')}</button></div>
    </Modal>
  );
}

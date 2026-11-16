// Forge Player: browse games published to a Forge Server and join them.

import { useEffect, useRef, useState } from 'react';
import { Viewport } from '../render/Viewport';
import { OutputLog } from '../editor/Output';
import { AssetStore } from '../assets/AssetStore';
import { parseFile } from '../core/Serializer';
import { PlaySession } from '../runtime/PlaySession';
import { NetClient } from '../net/NetClient';
import { ServerApi, normalizeServerUrl, type GameInfo } from '../net/api';
import { currentLanguage, languageChanged, languages, setLanguage, t } from '../i18n';
import { native } from '../project/ProjectIO';
import { Icon } from '../ui/icons';

const store = { get: (k: string, d = '') => { try { return localStorage.getItem(k) ?? d; } catch { return d; } }, set: (k: string, v: string) => { try { localStorage.setItem(k, v); } catch { /* ignore */ } } };

type Screen = { kind: 'browse' } | { kind: 'loading'; game: GameInfo; step: string } | { kind: 'playing'; game: GameInfo } | { kind: 'error'; message: string };

export function PlayerRoot() {
  const [lang, setLang] = useState(0);
  useEffect(() => languageChanged.connect(() => setLang((x) => x + 1)), []);
  return <PlayerApp key={lang} />;
}

function PlayerApp() {
  const [server, setServer] = useState(store.get('forge.player.server', 'http://127.0.0.1:8787'));
  const [name, setName] = useState(store.get('forge.player.name', `Player${Math.floor(Math.random() * 9000 + 1000)}`));
  const [games, setGames] = useState<GameInfo[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [screen, setScreen] = useState<Screen>({ kind: 'browse' });
  const [selected, setSelected] = useState<GameInfo | null>(null);
  const [joinId, setJoinId] = useState('');
  const gameRef = useRef<{ session: PlaySession; net: NetClient; vp: Viewport; output: OutputLog } | null>(null);

  const request = useRef(0);
  const refresh = () => {
    const n = ++request.current; // only the newest request may update the list
    store.set('forge.player.server', normalizeServerUrl(server));
    new ServerApi(server).games()
      .then((g) => { if (n === request.current) { setError(null); setGames(g.filter((x) => x.live)); } })
      .catch((e) => { if (n === request.current) { setGames([]); setError((e as Error).message); } });
  };
  // debounced while typing the address, then every 15 s
  useEffect(() => {
    const first = setTimeout(refresh, 400);
    const id = setInterval(refresh, 15000);
    return () => { clearTimeout(first); clearInterval(id); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [server]);

  const leave = () => {
    const g = gameRef.current;
    if (g) { g.session.stop(); g.net.close(); g.vp.dispose(); gameRef.current = null; }
    setScreen({ kind: 'browse' });
    refresh();
  };

  const play = async (game: GameInfo) => {
    const nm = name.trim() || 'Player';
    store.set('forge.player.name', nm);
    const api = new ServerApi(server);
    try {
      setScreen({ kind: 'loading', game, step: t('Downloading game…') });
      const { text, version } = await api.project(game.id);
      const file = parseFile(text);
      if (file.format !== 'forge-project') throw new Error(t('The server sent an invalid game file.'));
      setScreen({ kind: 'loading', game, step: t('Connecting to the server…') });
      const net = new NetClient(api, game.id, nm, version);
      await net.connect();
      setScreen({ kind: 'playing', game });
      // the viewport host element is rendered by <GameView/> after the state change
      let host: HTMLElement | null = null;
      for (let i = 0; i < 100 && !(host = document.getElementById('game-host')); i++) await new Promise((r) => setTimeout(r, 10));
      if (!host) throw new Error(t('Game view could not be created.'));
      const holder = document.createElement('div');
      holder.className = 'viewport-holder';
      host.prepend(holder);
      const vp = new Viewport(holder);
      vp.showGrid = false;
      const output = new OutputLog();
      const assets = new AssetStore(file.assets);
      const source = { scene: file.scene, assets, name: game.name, key: game.id };
      const startSession = async () => {
        const session = new PlaySession({ viewport: vp, output, notify: () => {} }, source, net);
        gameRef.current = { session, net, vp, output };
        (window as unknown as { forgePlayer: unknown }).forgePlayer = gameRef.current; // handy for debugging in DevTools
        await session.start();
        // the previous host left and we were elected: restart as host with a fresh world
        session.becameHost.connect(async () => {
          output.system(t('You are now hosting this server. The world was restarted.'));
          session.stop(false);
          const next = await startSession();
          next.broadcastSnapshot();
        });
        return session;
      };
      await startSession();
      net.closed.connect((reason) => { if (gameRef.current?.net === net) { leave(); setScreen({ kind: 'error', message: reason }); } });
      setScreen({ kind: 'playing', game });
    } catch (e) {
      const g = gameRef.current;
      if (g) { g.session.stop(); g.vp.dispose(); gameRef.current = null; }
      setScreen({ kind: 'error', message: (e as Error).message });
    }
  };

  if (screen.kind === 'loading') {
    return (
      <div className="player-loading">
        {screen.game.thumbnail && <div className="player-loading-bg" style={{ backgroundImage: `url(${screen.game.thumbnail})` }} />}
        <div className="player-loading-card">
          <h1>{screen.game.name}</h1>
          <p>{t('by {author}', { author: screen.game.author || t('Anonymous') })}</p>
          <div className="boot-bar"><i /></div>
          <span>{screen.step}</span>
        </div>
      </div>
    );
  }
  if (screen.kind === 'playing') return <GameView game={screen.game} gameRef={gameRef} onLeave={leave} />;

  return (
    <div className="player">
      <header className="player-top">
        <div className="titlebar-brand"><svg width="22" height="22" viewBox="0 0 16 16"><path d="M8 1 14.5 4.8v6.4L8 15 1.5 11.2V4.8z" fill="#2f7cf6" /><path d="M5.2 5h5.6v1.6H7v1.2h3.2v1.6H7V12H5.2z" fill="#fff" /></svg><b>Forge</b> Player</div>
        <div className="spacer" />
        <label className="player-field"><Icon name="player" size={14} /><input value={name} maxLength={24} onChange={(e) => setName(e.target.value)} title={t('Your display name')} /></label>
        <label className="player-field wide"><Icon name="world" size={14} /><input value={server} onChange={(e) => setServer(e.target.value)} onBlur={refresh} onKeyDown={(e) => { if (e.key === 'Enter') refresh(); }} title={t('Server address')} /></label>
        <button className="btn" onClick={refresh}><Icon name="restart" size={13} /> {t('Refresh')}</button>
        <select className="player-lang" value={currentLanguage()} onChange={(e) => setLanguage(e.target.value)}>
          {languages().map((l) => <option key={l.code} value={l.code}>{l.name}</option>)}
        </select>
      </header>
      {screen.kind === 'error' && <div className="player-banner error"><Icon name="error" size={14} /> {screen.message}<button className="link" onClick={() => setScreen({ kind: 'browse' })}>{t('OK')}</button></div>}
      <main className="player-main">
        <div className="player-heading">
          <h1>{t('Games')}</h1>
          <div className="row">
            <input className="text-input" placeholder={t('Game ID')} value={joinId} onChange={(e) => setJoinId(e.target.value)} style={{ width: 140 }} />
            <button className="btn" disabled={!joinId.trim()} onClick={() => new ServerApi(server).game(joinId.trim()).then((g) => play(g)).catch((e) => setScreen({ kind: 'error', message: (e as Error).message }))}>{t('Join by ID')}</button>
          </div>
        </div>
        {error && <div className="player-banner error"><Icon name="warn" size={14} /> {error} {native && <button className="link" onClick={() => native!.serverStart(8787).then(() => setTimeout(refresh, 800))}>{t('Start a local server')}</button>}</div>}
        {games === null ? <div className="home-empty">{t('Loading…')}</div>
          : games.length === 0 ? <div className="home-empty">{t('No games are published on this server yet. Publish one from Forge Studio (File > Publish to Server).')}</div>
            : (
              <div className="player-grid">
                {games.map((g) => (
                  <button key={g.id} className={'player-card' + (selected?.id === g.id ? ' selected' : '')} onClick={() => setSelected(g)} onDoubleClick={() => play(g)}>
                    <div className="player-card-art" style={g.thumbnail ? { backgroundImage: `url(${g.thumbnail})` } : undefined}>{!g.thumbnail && <Icon name="game" size={40} />}
                      {(g.playersOnline ?? 0) > 0 && <span className="player-online"><span className="dot" />{t('{n} playing', { n: g.playersOnline! })}</span>}
                    </div>
                    <div className="player-card-title">{g.name}</div>
                    <div className="player-card-meta">{g.author || t('Anonymous')} · v{g.live}</div>
                  </button>
                ))}
              </div>
            )}
      </main>
      {selected && (
        <aside className="player-detail">
          <div className="player-detail-art" style={selected.thumbnail ? { backgroundImage: `url(${selected.thumbnail})` } : undefined} />
          <h2>{selected.name}</h2>
          <p className="dim">{t('by {author}', { author: selected.author || t('Anonymous') })} · {t('Version {v}', { v: selected.live ?? '-' })} · {t('{n} playing', { n: selected.playersOnline ?? 0 })}</p>
          <p>{selected.description || t('No description.')}</p>
          <button className="btn primary big-play" onClick={() => play(selected)}><Icon name="play" size={18} /> {t('Play')}</button>
          <button className="link" onClick={() => setSelected(null)}>{t('Close')}</button>
        </aside>
      )}
    </div>
  );
}

function GameView({ game, gameRef, onLeave }: { game: GameInfo; gameRef: React.MutableRefObject<{ session: PlaySession; net: NetClient; vp: Viewport; output: OutputLog } | null>; onLeave: () => void }) {
  const [chat, setChat] = useState<{ id: number; name: string; text: string; time: number }[]>([]);
  const [typing, setTyping] = useState(false);
  const [draft, setDraft] = useState('');
  const [showPlayers, setShowPlayers] = useState(false);
  const [menu, setMenu] = useState(false);
  const [devConsole, setDevConsole] = useState(false);
  const [, tick] = useState(0);
  const inputRef = useRef<HTMLInputElement>(null);

  // re-subscribe whenever the session object changes (it is created after the first render, and again on host migration)
  const current = gameRef.current;
  useEffect(() => {
    const g = current;
    if (!g) return;
    let n = 0;
    const offs = [
      g.net.chat.connect((m) => setChat((c) => [...c.slice(-40), { id: ++n, name: m.name, text: m.text, time: Date.now() }])),
      g.net.joined.connect((p) => setChat((c) => [...c.slice(-40), { id: ++n, name: '', text: t('{name} joined the game.', { name: p.name }), time: Date.now() }])),
      g.net.left.connect(() => tick((x) => x + 1)),
      g.output.changed.connect(() => tick((x) => x + 1)),
    ];
    const timer = setInterval(() => tick((x) => x + 1), 1000);
    const key = (e: KeyboardEvent) => {
      if (typing) return;
      if (e.key === 'Tab') { e.preventDefault(); setShowPlayers((s) => !s); }
      if (e.key === 'F9') { e.preventDefault(); setDevConsole((s) => !s); }
      if (e.key === '/' || (e.key === 'Enter' && !menu)) { e.preventDefault(); setTyping(true); gameRef.current?.session.releaseKeys(); if (gameRef.current) gameRef.current.session.inputEnabled = false; setTimeout(() => inputRef.current?.focus()); }
      if (e.key === 'Escape') setMenu((m) => !m);
    };
    window.addEventListener('keydown', key);
    return () => { offs.forEach((o) => o()); clearInterval(timer); window.removeEventListener('keydown', key); };
  }, [current, typing, menu]);

  const send = () => {
    gameRef.current?.session.chat(draft);
    setDraft('');
    setTyping(false);
    if (gameRef.current) gameRef.current.session.inputEnabled = true;
    gameRef.current?.vp.canvas.focus();
  };
  const g = gameRef.current;
  const roster = g?.session.roster() ?? [];
  const visibleChat = chat.filter((c) => typing || Date.now() - c.time < 12000);

  return (
    <div className="game" id="game-host">
      <div className="game-hud-top">
        <button className="hud-btn" onClick={() => setMenu(true)} title={t('Menu (Esc)')}><Icon name="layout" size={16} /></button>
        <span className="hud-pill"><Icon name="world" size={12} /> {game.name}</span>
        <span className="hud-pill" title={t('Ping')}>{g?.net.rtt ?? 0} ms</span>
        <span className="hud-pill" title={t('Players (Tab)')}><Icon name="players" size={12} /> {roster.length}</span>
        {g?.session.role === 'host' && <span className="hud-pill host">{t('Host')}</span>}
      </div>
      <div className="game-chat">
        {visibleChat.map((c) => <div key={c.id} className="chat-line">{c.name && <b>{c.name}: </b>}{c.text}</div>)}
        {typing && (
          <input ref={inputRef} className="chat-input" value={draft} maxLength={200} placeholder={t('Say something… (Enter to send, Esc to cancel)')}
            onChange={(e) => setDraft(e.target.value)}
            onKeyDown={(e) => {
              e.stopPropagation();
              if (e.key === 'Enter') send();
              if (e.key === 'Escape') { setTyping(false); setDraft(''); if (gameRef.current) gameRef.current.session.inputEnabled = true; }
            }} />
        )}
        {!typing && <div className="chat-hint">{t('Press / to chat · Tab: players · F9: console')}</div>}
      </div>
      {showPlayers && (
        <div className="player-list glass">
          <div className="player-list-title">{t('Players')}</div>
          {roster.map((p) => <div key={p.id} className={'player-list-row' + (p.me ? ' me' : '')}><span>{p.name}</span>{p.host && <span className="badge">{t('Host')}</span>}{p.stats.map(([k, v]) => <span key={k} className="dim">{k}: {String(v)}</span>)}</div>)}
        </div>
      )}
      {devConsole && g && (
        <div className="dev-console glass">
          <div className="player-list-title">{t('Developer console (F9)')}</div>
          <div className="log">{g.output.entries.slice(-80).map((e) => <div key={e.id} className={`log-line log-${e.level}`}><span className="log-msg">{e.message}</span></div>)}</div>
        </div>
      )}
      {menu && (
        <div className="game-menu-backdrop" onClick={() => setMenu(false)}>
          <div className="game-menu glass" onClick={(e) => e.stopPropagation()}>
            <h2>{game.name}</h2>
            <button className="btn primary wide" onClick={() => setMenu(false)}>{t('Resume')}</button>
            <button className="btn wide" onClick={() => { setMenu(false); setShowPlayers(true); }}>{t('Players')}</button>
            <button className="btn danger wide" onClick={onLeave}>{t('Leave game')}</button>
          </div>
        </div>
      )}
    </div>
  );
}

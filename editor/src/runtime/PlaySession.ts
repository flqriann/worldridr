// A running game. Works on an isolated copy of the scene (serialize ->
// deserialize), so nothing that happens while playing changes the project.
//
// Roles:
//  * local  – Studio play test: everything runs here, single player
//  * host   – multiplayer, first player in a session: runs server Scripts and
//             physics, replicates the world to the other clients
//  * client – multiplayer: runs LocalScripts and its own character only,
//             applies the host's replication
//
// Owns physics, scripts, characters (own + remote puppets), camera, input,
// audio, animations, the in-game GUI and the leaderboard.

import * as THREE from 'three';
import type { DataModel, Instance } from '../core/Instance';
import { Instance as Inst } from '../core/Instance';
import { LoadReport, deserializeScene, serializeScene, type SceneJSON } from '../core/Serializer';
import { v3, type Vec3 } from '../core/types';
import { Signal } from '../core/Signal';
import { Studio } from '../editor/Studio';
import { Selection } from '../editor/Selection';
import type { OutputLog } from '../editor/Output';
import type { Viewport } from '../render/Viewport';
import type { AssetStore } from '../assets/AssetStore';
import { LuaRuntime, type Enc, type RuntimeHost } from './LuaRuntime';
import { Physics } from './Physics';
import { Character } from './Character';
import { AudioSystem } from './Audio';
import { keyCodeName } from './enums';
import { effectiveSize, orientationToQuat, quatToOrientation } from '../render/geometry';
import { sampleAnimation, validateAnimation, type AnimationClip } from '../assets/AssetStore';
import type { NetClient, NetMessage } from '../net/NetClient';
import { Replicator, type RepOp } from '../net/Replicator';
import type { InstanceJSON } from '../core/Serializer';
import { t } from '../i18n';

interface AnimState { inst: Instance; clip: AnimationClip; t: number; targets: { part: Instance; base: THREE.Matrix4 }[]; pivot: THREE.Matrix4 }

export interface SessionHost {
  viewport: Viewport;
  output: OutputLog;
  /** UI refresh request (state, errors, players changed). */
  notify(): void;
}
export interface SessionSource { scene: SceneJSON; assets: AssetStore; name: string; key: string }

export type SessionRole = 'local' | 'host' | 'client';

export class PlaySession {
  dm!: DataModel;
  readonly selection = new Selection();
  rt: LuaRuntime | null = null;
  physics!: Physics;
  audio: AudioSystem;
  /** Our own Player instance. */
  player!: Instance;
  character: Character | null = null;
  paused = false;
  time = 0;
  errors = 0;
  readonly changed = new Signal();
  /** Multiplayer: we became the host after the previous host left (the app restarts the session as host). */
  readonly becameHost = new Signal();
  readonly assets: AssetStore;
  readonly role: SessionRole;
  private myId: number;
  private players = new Map<number, Instance>();
  private puppets = new Map<number, Character>();
  private replicator: Replicator | null = null;
  private netTimer = 0;
  private unsub: (() => void)[] = [];
  private keys = new Set<string>();
  private mouseButtons = new Set<number>();
  private camYaw = 0;
  private camPitch = -0.3;
  private camZoom = 14;
  private rmb: { x: number; y: number; moved: boolean } | null = null;
  private pointerLocked = false;
  private respawnTimer = -1;
  private anims = new Map<Instance, AnimState>();
  private respawnLocation = new WeakMap<Instance, Instance>();
  private leaderboard: HTMLDivElement | null = null;
  private lbTimer = 0;
  private stopped = false;
  private mouse = { x: 0, y: 0 };
  private downAt: { x: number; y: number } | null = null;
  private keyHandlers: { down: (e: KeyboardEvent) => void; up: (e: KeyboardEvent) => void; blur: () => void } | null = null;
  private dataStores = new Map<string, Instance>();
  /** When false, keyboard input is ignored (e.g. while typing in chat). */
  inputEnabled = true;

  constructor(private host: SessionHost, private source: SessionSource, readonly net: NetClient | null = null) {
    this.assets = source.assets;
    this.audio = new AudioSystem(source.assets);
    this.role = !net ? 'local' : net.isHost ? 'host' : 'client';
    this.myId = net ? net.id : 0;
  }

  get isHost() { return this.role !== 'client'; }
  private get vp() { return this.host.viewport; }

  private log(level: 'print' | 'warn' | 'error' | 'info', msg: string, scriptId?: string, line?: number) {
    if (level === 'error') { this.errors++; this.changed.fire(); this.host.notify(); }
    const script = scriptId ? this.dm?.byId(scriptId) : undefined;
    const src = scriptId && scriptId !== 'commandbar' ? { scriptId, line, name: script?.getFullName() } : undefined;
    this.host.output.log(level === 'print' ? 'print' : level, msg, src);
  }

  async start() {
    const vp = this.vp;
    // 1. isolated copy of the scene (ids preserved so errors map to the edit scripts and ids match across clients)
    const report = new LoadReport();
    this.dm = deserializeScene(this.source.scene, report);
    for (const w of report.warnings) this.log('warn', w);
    this.selection.bind(this.dm);
    const dm = this.dm;

    // 2. players
    const myName = this.net ? this.net.name : 'Player1';
    this.player = this.createPlayer(this.myId, myName, true);
    if (this.net) for (const [id, name] of this.net.players) if (id !== this.myId) this.createPlayer(id, name, false);

    // 3. physics + viewport
    this.physics = new Physics(dm, this.isHost);
    this.physics.build();
    this.physics.onTouch = (a, b, began) => {
      const rt = this.rt;
      if (!rt) return;
      const ev = began ? 'Touched' : 'TouchEnded';
      rt.fire(a, ev, rt.encInst(b));
      rt.fire(b, ev, rt.encInst(a));
    };
    vp.setDataModel(dm, null, this.assets, 'play');
    vp.externalCamera = true;
    vp.playPointer = (e, kind) => this.onPointer(e, kind);
    this.bindInput();
    this.audio.masterVolume = dm.getService('SoundService')!.props.Volume as number;
    this.audio.onEnded = (s) => this.rt?.fire(s, 'Ended', this.rt.encInst(s));
    this.audio.onError = (m) => this.log('warn', m);

    // 4. networking
    if (this.net) this.bindNet();

    // 5. scripts
    this.log('info', this.role === 'local'
      ? t('Test started – {n} objects. WASD/arrows to move, Space to jump, right-drag to turn the camera, wheel to zoom.', { n: dm.index.size })
      : t('Joined "{name}" as {role} – {n} players online.', { name: this.source.name, role: this.role === 'host' ? t('host') : t('player'), n: this.players.size }));
    this.rt = await LuaRuntime.create(dm, this.runtimeHost());
    if (this.stopped) { this.rt.dispose(); return; }
    this.bindScene();
    for (const d of dm.getDescendants()) if (this.isRunnableScript(d)) this.rt.runScript(d);
    const playersSvc = dm.getService('Players')!;
    for (const p of this.players.values()) this.rt.fire(playersSvc, 'PlayerAdded', this.rt.encInst(p));
    const pgui = this.player.findFirstChildOfClass('PlayerGui')!;
    vp.gui.setContainer(dm, pgui, true);
    vp.gui.onButtonClick = (b) => this.rt?.fire(b, 'MouseButton1Click');
    vp.gui.onMouseEnter = (b, enter) => this.rt?.fire(b, enter ? 'MouseEnter' : 'MouseLeave');
    this.spawnCharacter();
    for (const d of dm.getDescendants()) {
      if (d.className === 'Sound' && d.props.Playing) this.audio.play(d);
      if (d.className === 'KeyframeAnimation' && d.props.AutoPlay && this.isHost) this.startAnim(d);
    }
    this.buildLeaderboard();
    this.unsub.push(vp.beforeRender.connect(this.frame));
    this.changed.fire();
  }

  /** Creates a Player instance (deterministic id "player-<id>"); only our own gets GUI + scripts. */
  private createPlayer(id: number, name: string, local: boolean): Instance {
    const dm = this.dm;
    const players = dm.getService('Players')!;
    const existing = dm.byId(`player-${id}`);
    if (existing) return existing;
    const player = new Inst('Player', `player-${id}`);
    player.props.Name = name;
    player.props.DisplayName = name;
    player.props.UserId = id;
    player.runtimeOnly = true;
    if (local) {
      const pgui = new Inst('PlayerGui'); pgui.props.Name = 'PlayerGui'; pgui.runtimeOnly = true;
      pgui.parent = player; player.children.push(pgui);
      for (const g of dm.getService('StarterGui')!.children) { const c = g.clone(); c.parent = pgui; pgui.children.push(c); }
      const ps = new Inst('Folder'); ps.props.Name = 'PlayerScripts'; ps.runtimeOnly = true;
      ps.parent = player; player.children.push(ps);
      const sps = dm.getService('StarterPlayer')!.findFirstChildOfClass('StarterPlayerScripts');
      for (const s of sps?.children ?? []) { const c = s.clone(); c.parent = ps; ps.children.push(c); }
    }
    player.setParent(players);
    this.players.set(id, player);
    return player;
  }

  private playerId(p: Instance | null | undefined): number | null {
    const m = p?.id.match(/^player-(\d+)$/);
    return m ? +m[1] : null;
  }

  private isRunnableScript(d: Instance): boolean {
    if (!d.dm || d.props.Enabled === false) return false;
    const dm = this.dm;
    if (d.className === 'Script') return this.isHost && (d.isDescendantOf(dm.workspace) || d.isDescendantOf(dm.getService('ServerScriptService')!));
    if (d.className === 'LocalScript') return d.isDescendantOf(this.player) || (!!this.character && d.isDescendantOf(this.character.model));
    return false;
  }

  // ---- networking -----------------------------------------------------------------

  private bindNet() {
    const net = this.net!;
    this.replicator = new Replicator(this.dm);
    if (this.role === 'host') this.replicator.startHost();
    this.unsub.push(
      net.joined.connect((p) => {
        const pl = this.createPlayer(p.id, p.name, false);
        this.rt?.fire(this.dm.getService('Players')!, 'PlayerAdded', this.rt.encInst(pl));
        if (this.role === 'host') {
          // send the current world (flush pending ops first so the snapshot is complete)
          this.sendRep();
          net.send({ t: 'repfull', to: p.id, items: this.replicator!.snapshot() });
        }
        this.log('info', t('{name} joined the game.', { name: p.name }));
        this.host.notify();
      }),
      net.left.connect((id) => {
        const pl = this.players.get(id);
        if (pl) this.rt?.fire(this.dm.getService('Players')!, 'PlayerRemoving', this.rt.encInst(pl));
        const puppet = this.puppets.get(id);
        if (puppet) { puppet.dispose(); puppet.model.setParent(null); this.puppets.delete(id); }
        if (pl) { pl.setParent(null); this.players.delete(id); this.log('info', t('{name} left the game.', { name: pl.name })); }
        this.host.notify();
      }),
      net.hostChanged.connect((id) => { if (id === this.myId && this.role === 'client') this.becameHost.fire(); }),
      net.message.connect((m) => this.onNetMessage(m)),
    );
  }

  private onNetMessage(m: NetMessage) {
    switch (m.t) {
      case 'state': {
        const id = m.from!;
        if (id === this.myId || !this.players.has(id)) return;
        let puppet = this.puppets.get(id);
        const dead = !!m.d;
        if (puppet && puppet.dead && !dead) { puppet.dispose(); puppet.model.setParent(null); this.puppets.delete(id); puppet = undefined; }
        if (!puppet) puppet = this.createPuppet(id) ?? undefined;
        if (!puppet) return;
        if (dead && !puppet.dead) { puppet.die(); return; }
        puppet.setRemoteState(m.p as [number, number, number], Number(m.y) || 0, Number(m.s) || 0);
        if (typeof m.h === 'number' && puppet.humanoid.props.Health !== m.h) puppet.humanoid.setSilent('Health', m.h);
        break;
      }
      case 'rep':
        if (this.role === 'client' && m.from === this.net!.host) {
          const moved = this.replicator!.apply(m.ops as RepOp[]);
          if (moved.length) this.physics.syncFromInstances(moved);
        }
        break;
      case 'repfull':
        if (this.role === 'client') {
          this.replicator!.applySnapshot(m.items as { parent: string; json: InstanceJSON }[]);
          this.physics.build();
          this.log('info', t('World synchronized with the host.'));
        }
        break;
      case 'rpc': this.onRpc(m); break;
    }
  }

  private createPuppet(id: number): Character | null {
    const pl = this.players.get(id);
    if (!pl) return null;
    const ch = new Character(pl.name, this.physics, this.dm.getService('StarterPlayer')!, () => null, { idPrefix: `char-${id}-`, puppet: true });
    ch.model.setParent(this.dm.workspace);
    this.puppets.set(id, ch);
    return ch;
  }

  private onRpc(m: NetMessage) {
    const rt = this.rt;
    if (!rt) return;
    if (m.name === 'click' && this.isHost) {
      const cd = this.dm.byId(String(m.id));
      const from = this.players.get(m.from!);
      if (cd?.className === 'ClickDetector' && from) rt.fire(cd, 'MouseClick', rt.encInst(from));
      return;
    }
    if (m.name !== 're') return;
    const re = this.dm.byId(String(m.id));
    if (!re || re.className !== 'RemoteEvent') return;
    const args = Array.isArray(m.args) ? (m.args as Enc[]) : [];
    if (m.dir === 'server' && this.isHost) {
      const from = this.players.get(m.from!);
      if (from) rt.fire(re, 'OnServerEvent', rt.encInst(from), ...args);
    } else if (m.dir === 'client') {
      rt.fire(re, 'OnClientEvent', ...args);
    }
  }

  private sendRep() {
    if (this.role !== 'host' || !this.replicator) return;
    const ops = this.replicator.flush();
    if (ops.length) this.net!.send({ t: 'rep', ops });
  }

  private sendState() {
    const ch = this.character;
    if (!ch || !this.net) return;
    const p = ch.position;
    this.net.send({ t: 'state', p: [r3(p.x), r3(p.y), r3(p.z)], y: r3(ch.yaw), s: r3(ch.currentSwing), d: ch.dead ? 1 : 0, h: ch.humanoid.props.Health as number });
  }

  /** After a host migration: send the (new) world to every other player. */
  broadcastSnapshot() {
    if (this.role !== 'host' || !this.net || !this.replicator) return;
    const items = this.replicator.snapshot();
    for (const id of this.players.keys()) if (id !== this.myId) this.net.send({ t: 'repfull', to: id, items });
  }

  /** Sends a chat message (multiplayer) or prints it locally. */
  chat(text: string) {
    const s = text.trim().slice(0, 200);
    if (!s) return;
    if (this.net) this.net.send({ t: 'chat', text: s });
    else this.log('print', `[${this.player.name}] ${s}`);
  }

  /** Players with their leaderstats for HUDs. */
  roster(): { id: number; name: string; me: boolean; host: boolean; stats: [string, unknown][] }[] {
    return [...this.players.entries()].map(([id, p]) => {
      const stats = p.findFirstChild('leaderstats')?.children.filter((c) => c.className.endsWith('Value')).map((c) => [c.name, c.props.Value] as [string, unknown]) ?? [];
      return { id, name: p.name, me: id === this.myId, host: this.net ? this.net.host === id : true, stats };
    });
  }

  // ---- scene listeners -------------------------------------------------------------

  private bindScene() {
    const dm = this.dm;
    this.unsub.push(
      dm.added.connect((i) => {
        if (i.isA('BasePart') && i.isDescendantOf(dm.workspace) && !this.physics.kinematic.has(i)) this.physics.add(i);
        if (this.isRunnableScript(i)) this.rt?.runScript(i);
        if (i.className === 'Sound' && i.props.Playing) this.audio.play(i);
        if (i.className === 'KeyframeAnimation' && i.props.AutoPlay && this.isHost) this.startAnim(i);
      }),
      dm.removed.connect((i) => {
        if (i.isA('BasePart')) this.physics.remove(i);
        if (i.className === 'Sound') this.audio.stop(i, false);
        if (i.className === 'KeyframeAnimation') this.anims.delete(i);
      }),
      dm.reparented.connect((i) => {
        i.walk((d) => {
          if (!d.isA('BasePart') || this.physics.kinematic.has(d)) return;
          if (d.isDescendantOf(dm.workspace)) { if (!this.physics.bodies.has(d)) this.physics.add(d); }
          else this.physics.remove(d);
        });
      }),
      dm.changed.connect((i, prop) => {
        if (i.isA('BasePart')) this.physics.onPropChanged(i, prop);
        else if (i.className === 'Sound') {
          if (prop === 'Playing') { if (i.props.Playing) { if (!this.audio.isPlaying(i)) this.audio.play(i); } else this.audio.stop(i, false); }
          else this.audio.refresh(i);
        } else if (i.className === 'Humanoid' && prop === 'Health') this.onHealth(i);
        else if ((i.className === 'Script' || i.className === 'LocalScript') && prop === 'Enabled') {
          if (i.props.Enabled) { if (this.isRunnableScript(i)) this.rt?.runScript(i); } else this.rt?.stopScript(i);
        } else if (i.className === 'SoundService' && prop === 'Volume') this.audio.masterVolume = i.props.Volume as number;
      }),
    );
    if (this.role === 'client') this.unsub.push(dm.transformsChanged.connect((list) => this.physics.syncFromInstances(list)));
  }

  // ---- script host -------------------------------------------------------------------

  private runtimeHost(): RuntimeHost {
    return {
      log: (l, m, s, line) => this.log(l, m, s, line),
      getProp: (inst, key) => {
        const rt = this.rt!;
        if (inst.className === 'Players' && key === 'LocalPlayer') return { value: rt.encInst(this.player) };
        if (inst.className === 'Player') {
          if (key === 'Character') {
            const pid = this.playerId(inst);
            const ch = pid === this.myId ? this.character : pid !== null ? this.puppets.get(pid) : undefined;
            return { value: rt.encInst(ch && ch.model.parent ? ch.model : null) };
          }
          if (key === 'RespawnLocation') return { value: rt.encInst(this.respawnLocation.get(inst)) };
        }
        if (inst.isA('BasePart') && (key === 'AssemblyLinearVelocity' || key === 'Velocity')) {
          const v = this.physics.getVelocity(inst);
          return { value: rt.encV3(v ?? (inst.props.AssemblyLinearVelocity as Vec3)) };
        }
        if (inst.className === 'Humanoid' && key === 'RootPart') {
          const root = inst.parent?.findFirstChild('HumanoidRootPart');
          return { value: rt.encInst(root ?? null) };
        }
        return undefined;
      },
      setProp: (inst, key, value) => {
        const rt = this.rt!;
        if (inst.className === 'Player' && key === 'RespawnLocation') {
          const p = rt.decodeInst(value);
          if (p) this.respawnLocation.set(inst, p); else this.respawnLocation.delete(inst);
          return true;
        }
        if (inst.isA('BasePart') && (key === 'AssemblyLinearVelocity' || key === 'Velocity')) {
          const v = rt.decodeVec3(value, `Unable to assign property ${key}`);
          if (this.character && inst === this.character.root) this.character.velocity.set(v.x, v.y, v.z);
          else { inst.setSilent('AssemblyLinearVelocity', v); this.physics.setVelocity(inst, v); }
          return true;
        }
        if (this.character && inst === this.character.root && (key === 'Position' || key === 'CFrame')) {
          const p = key === 'CFrame' ? rt.decodeCFrame(value).pos : rt.decodeVec3(value, 'Unable to assign property Position');
          this.character.teleport(p);
          return true;
        }
        return false;
      },
      call: (inst, method, args) => this.hostCall(inst, method, args),
      raycast: (o, d, filter, mode) => {
        const rt = this.rt!;
        const hit = this.physics.raycast(o, d, new Set(filter), mode);
        if (!hit) return undefined;
        const mat = hit.inst.className === 'Terrain' ? this.terrainMaterialAt(hit.point) : (hit.inst.props.Material as string) ?? 'Plastic';
        return ['RR', rt.encInst(hit.inst), rt.encV3(hit.point), rt.encV3(hit.normal), ['E', 'Material', mat], hit.distance];
      },
    };
  }

  private terrainMaterialAt(p: Vec3): string {
    const tt = this.dm.terrainInstance?.terrain;
    if (!tt) return 'Grass';
    const ix = Math.round(tt.gridX(p.x)), iz = Math.round(tt.gridZ(p.z));
    if (!tt.inBounds(ix, iz)) return 'Grass';
    const names = ['Air', 'Grass', 'Dirt', 'Sand', 'Rock', 'Snow', 'Mud', 'Ice'];
    return names[tt.materials[tt.idx(ix, iz)]] ?? 'Grass';
  }

  private hostCall(inst: Instance, method: string, args: unknown[]): { value: unknown } | undefined {
    const rt = this.rt!;
    switch (method) {
      case 'GetPlayerFromCharacter': {
        const ch = rt.decodeInst(args[0]);
        const m = ch?.id.match(/^char-(\d+)-Model$/);
        return { value: rt.encInst(m ? this.players.get(+m[1]) : null) };
      }
      case 'LoadCharacter': if (inst === this.player) { this.spawnCharacter(); return { value: undefined }; } break;
      case 'GetMouse': throw new Error('Player:GetMouse() is not supported – use UserInputService.InputBegan / GetMouseLocation');
      case 'TakeDamage':
        if (inst.className === 'Humanoid') { inst.set('Health', Math.max(0, (inst.props.Health as number) - Number(args[0] ?? 0))); return { value: undefined }; }
        break;
      case 'MoveTo':
        if (inst.className === 'Humanoid') throw new Error('Humanoid:MoveTo is not supported for player characters in this version');
        break;
      case 'Jump':
        if (inst.className === 'Humanoid' && this.character?.humanoid === inst && this.character.grounded) this.character.velocity.y = inst.props.JumpPower as number;
        return { value: undefined };
      case 'GetMass': return { value: this.physics.getMass(inst) };
      case 'ApplyImpulse': {
        const v = rt.decodeVec3(args[0], 'ApplyImpulse');
        if (this.character && inst === this.character.root) { const cv = this.character.velocity; const mass = 70; cv.x += v.x / mass; cv.y += v.y / mass; cv.z += v.z / mass; }
        else this.physics.applyImpulse(inst, v);
        return { value: undefined };
      }
      case 'GetTouchingParts': return { value: ['T', ...this.physics.touchingParts(inst).map((p) => rt.encInst(p))] };
      case 'Play':
        if (inst.className === 'Sound') { inst.setSilent('Playing', true); this.audio.play(inst); rt.fire(inst, 'Played', rt.encInst(inst)); return { value: undefined }; }
        if (inst.className === 'KeyframeAnimation') { this.startAnim(inst); return { value: undefined }; }
        break;
      case 'Stop':
        if (inst.className === 'Sound') { this.audio.stop(inst); return { value: undefined }; }
        if (inst.className === 'KeyframeAnimation') { this.stopAnim(inst); return { value: undefined }; }
        break;
      case 'Pause': if (inst.className === 'Sound') { this.audio.pause(inst); return { value: undefined }; } break;
      case 'Resume': if (inst.className === 'Sound') { this.audio.resume(inst); return { value: undefined }; } break;
      case 'Emit': if (inst.className === 'ParticleEmitter') { this.vp.emitter(inst)?.emit(Number(args[0] ?? 16)); return { value: undefined }; } break;
      case 'Clear': if (inst.className === 'ParticleEmitter') { this.vp.emitter(inst)?.clear(); return { value: undefined }; } break;
      case 'IsKeyDown': {
        const k = Array.isArray(args[0]) ? String(args[0][2]) : String(args[0]);
        return { value: [...this.keys].some((c) => keyCodeName(c) === k) };
      }
      case 'IsMouseButtonPressed': {
        const tt = Array.isArray(args[0]) ? String(args[0][2]) : String(args[0]);
        return { value: this.mouseButtons.has(tt === 'MouseButton2' ? 2 : tt === 'MouseButton3' ? 1 : 0) };
      }
      case 'GetMouseLocation': return { value: ['V2', this.mouse.x, this.mouse.y] };

      // ---- RemoteEvent
      case 'FireServer': {
        if (inst.className !== 'RemoteEvent') break;
        if (this.isHost) rt.fire(inst, 'OnServerEvent', rt.encInst(this.player), ...(args as Enc[]));
        else this.net!.send({ t: 'rpc', to: 'host', name: 're', dir: 'server', id: inst.id, args });
        return { value: undefined };
      }
      case 'FireClient': {
        if (inst.className !== 'RemoteEvent') break;
        if (!this.isHost) throw new Error('FireClient can only be called from a server Script');
        const target = rt.decodeInst(args[0]);
        const pid = this.playerId(target);
        if (pid === null) throw new Error('FireClient expects a Player as the first argument');
        const rest = args.slice(1) as Enc[];
        if (pid === this.myId) rt.fire(inst, 'OnClientEvent', ...rest);
        else this.net?.send({ t: 'rpc', to: pid, name: 're', dir: 'client', id: inst.id, args: rest });
        return { value: undefined };
      }
      case 'FireAllClients': {
        if (inst.className !== 'RemoteEvent') break;
        if (!this.isHost) throw new Error('FireAllClients can only be called from a server Script');
        rt.fire(inst, 'OnClientEvent', ...(args as Enc[]));
        this.net?.send({ t: 'rpc', to: 'all', name: 're', dir: 'client', id: inst.id, args });
        return { value: undefined };
      }

      // ---- DataStore
      case 'GetDataStore': {
        if (inst.className !== 'DataStoreService') break;
        const name = String(args[0] ?? '');
        if (!/^[\w\-. ]{1,64}$/.test(name)) throw new Error('GetDataStore expects a name of 1-64 letters, digits, - _ . or spaces');
        let ds = this.dataStores.get(name);
        if (!ds) { ds = new Inst('DataStore'); ds.props.Name = name; ds.runtimeOnly = true; this.dataStores.set(name, ds); }
        return { value: rt.encInst(ds) };
      }
      case 'GetAsync': case 'SetAsync': case 'IncrementAsync': case 'RemoveAsync': {
        if (inst.className !== 'DataStore') break;
        return { value: rt.startAsync(this.dataStoreOp(inst.name, method, args)) };
      }
    }
    return undefined;
  }

  // ---- DataStore -------------------------------------------------------------------

  private async dataStoreOp(store: string, op: string, args: unknown[]): Promise<Enc> {
    if (!this.isHost) throw new Error('DataStores can only be used from server Scripts');
    const key = String(args[0] ?? '');
    if (!/^[\w\-. ]{1,64}$/.test(key)) throw new Error('DataStore keys must be 1-64 letters, digits, - _ . or spaces');
    const backend = this.net
      ? {
        get: () => this.net!.api.dsGet(this.net!.gameId, store, key),
        set: (v: unknown) => this.net!.api.dsSet(this.net!.gameId, store, key, v, this.net!.welcome.token).then(() => undefined),
        remove: () => this.net!.api.dsRemove(this.net!.gameId, store, key, this.net!.welcome.token).then(() => undefined),
      }
      : localDataStore(this.source.key, store, key);
    switch (op) {
      case 'GetAsync': return jsonToLua(await backend.get());
      case 'SetAsync': await backend.set(luaToJson(args[1])); return undefined;
      case 'RemoveAsync': { const old = await backend.get(); await backend.remove(); return jsonToLua(old); }
      case 'IncrementAsync': {
        const old = await backend.get();
        const n = (typeof old === 'number' ? old : 0) + Number(args[1] ?? 1);
        await backend.set(n);
        return n;
      }
    }
    return undefined;
  }

  // ---- character / spawning -------------------------------------------------------------

  private spawnPoint(): { feet: Vec3; yaw: number } {
    const custom = this.respawnLocation.get(this.player);
    const spawns = this.dm.workspace.getDescendants().filter((d) => d.className === 'SpawnLocation' && d.props.Enabled !== false);
    const sp = custom && custom.dm ? custom : spawns[Math.floor(Math.random() * spawns.length)];
    if (sp) {
      const p = sp.props.Position as Vec3;
      const s = effectiveSize(sp);
      const yaw = ((sp.props.Orientation as Vec3).y * Math.PI) / 180;
      return { feet: v3(p.x + (Math.random() - 0.5) * Math.max(0, s.x - 4), p.y + s.y / 2 + 0.1, p.z + (Math.random() - 0.5) * Math.max(0, s.z - 4)), yaw };
    }
    const h = this.dm.terrainInstance?.terrain?.heightAt(0, 0);
    return { feet: v3(0, (h ?? 0) + 6, 0), yaw: 0 };
  }

  spawnCharacter() {
    const old = this.character;
    if (old) {
      if (this.rt) this.rt.fire(this.player, 'CharacterRemoving', this.rt.encInst(old.model));
      old.dispose();
      old.model.setParent(null);
    }
    const ch = new Character(this.player.name, this.physics, this.dm.getService('StarterPlayer')!, (x, z) => {
      const tr = this.dm.terrainInstance;
      return tr?.terrain?.waterAt(x, z) ? (tr.props.WaterLevel as number) : null;
    }, { idPrefix: `char-${this.myId}-` });
    ch.onJump = () => this.rt?.fire(ch.humanoid, 'Jumping', true);
    const sp = this.spawnPoint();
    ch.spawn(sp.feet, sp.yaw);
    this.character = ch;
    this.camYaw = sp.yaw;
    ch.model.setParent(this.dm.workspace);
    this.respawnTimer = -1;
    if (this.rt) this.rt.fire(this.player, 'CharacterAdded', this.rt.encInst(ch.model));
    this.sendState();
  }

  private onHealth(h: Instance) {
    this.rt?.fire(h, 'HealthChanged', h.props.Health as number);
    const ch = this.character;
    if (!ch || ch.humanoid !== h) return;
    const hp = h.props.Health as number;
    const max = h.props.MaxHealth as number;
    if (hp > max) { h.setSilent('Health', max); return; }
    if (hp <= 0 && !ch.dead) {
      ch.die();
      this.rt?.fire(h, 'Died');
      this.respawnTimer = Math.max(0, this.dm.getService('Players')!.props.RespawnTime as number);
      this.log('info', t('{name} died. Respawning in {s} s.', { name: this.player.name, s: this.respawnTimer.toFixed(0) }));
      this.sendState();
    }
  }

  // ---- animations -------------------------------------------------------------------

  private startAnim(inst: Instance) {
    const a = this.assets.get(inst.props.AnimationId as string);
    if (!a || a.kind !== 'animation') { this.log('warn', t('{name}: animation asset not found', { name: inst.getFullName() })); return; }
    let clip: AnimationClip;
    try { clip = validateAnimation(a.data); } catch (e) { this.log('error', `${inst.getFullName()}: ${(e as Error).message}`); return; }
    const target = inst.parent;
    if (!target) return;
    const parts = target.isA('BasePart') ? [target] : target.getDescendants().filter((d) => d.isA('BasePart'));
    if (!parts.length) { this.log('warn', t('{name}: parent has no parts to animate', { name: inst.getFullName() })); return; }
    const mat = (p: Instance) => new THREE.Matrix4().compose(new THREE.Vector3().copy(p.props.Position as THREE.Vector3Like), orientationToQuat(p.props.Orientation as Vec3), new THREE.Vector3(1, 1, 1));
    let pivot: THREE.Matrix4;
    if (target.isA('BasePart')) pivot = mat(target);
    else {
      const b = new THREE.Box3();
      for (const p of parts) b.expandByPoint(new THREE.Vector3().copy(p.props.Position as THREE.Vector3Like));
      pivot = new THREE.Matrix4().makeTranslation(b.getCenter(new THREE.Vector3()));
    }
    this.anims.set(inst, { inst, clip, t: 0, targets: parts.map((p) => ({ part: p, base: mat(p) })), pivot });
  }

  private stopAnim(inst: Instance) {
    if (this.anims.delete(inst)) this.rt?.fire(inst, 'Stopped');
  }

  private updateAnims(dt: number) {
    const pos = new THREE.Vector3(), q = new THREE.Quaternion(), s = new THREE.Vector3();
    for (const st of this.anims.values()) {
      if (!st.inst.dm) { this.anims.delete(st.inst); continue; }
      st.t += dt * (st.inst.props.Speed as number);
      const dur = Math.max(0.001, st.clip.duration);
      if (st.t > dur) {
        if (st.inst.props.Looped) st.t %= dur;
        else { st.t = dur; this.anims.delete(st.inst); this.rt?.fire(st.inst, 'Stopped'); }
      }
      const k = sampleAnimation(st.clip, st.t);
      const off = new THREE.Matrix4().compose(
        new THREE.Vector3(...k.position),
        new THREE.Quaternion().setFromEuler(new THREE.Euler(k.rotation[0] * Math.PI / 180, k.rotation[1] * Math.PI / 180, k.rotation[2] * Math.PI / 180, 'YXZ')),
        new THREE.Vector3(k.scale, k.scale, k.scale),
      );
      const delta = st.pivot.clone().multiply(off).multiply(st.pivot.clone().invert());
      for (const tg of st.targets) {
        if (!tg.part.dm) continue;
        delta.clone().multiply(tg.base).decompose(pos, q, s);
        tg.part.setSilent('Position', v3(pos.x, pos.y, pos.z));
        tg.part.setSilent('Orientation', quatToOrientation(q));
      }
    }
  }

  // ---- input -------------------------------------------------------------------------

  private bindInput() {
    const isTyping = (e: KeyboardEvent) => {
      const el = e.target as HTMLElement | null;
      return !this.inputEnabled || (!!el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || !!el.closest?.('.monaco-editor')));
    };
    const down = (e: KeyboardEvent) => {
      if (isTyping(e) || e.ctrlKey || e.metaKey) return;
      if (e.code === 'Escape' && this.pointerLocked) document.exitPointerLock();
      if (!this.keys.has(e.code)) {
        this.keys.add(e.code);
        this.fireInput('InputBegan', e.code);
      }
      if (['Space', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight'].includes(e.code)) e.preventDefault();
    };
    const up = (e: KeyboardEvent) => {
      if (this.keys.delete(e.code)) this.fireInput('InputEnded', e.code);
    };
    const blur = () => this.keys.clear();
    window.addEventListener('keydown', down);
    window.addEventListener('keyup', up);
    window.addEventListener('blur', blur);
    this.keyHandlers = { down, up, blur };
    const plc = () => { this.pointerLocked = document.pointerLockElement === this.vp.canvas; };
    document.addEventListener('pointerlockchange', plc);
    this.unsub.push(() => document.removeEventListener('pointerlockchange', plc));
  }

  /** Clears held keys (e.g. when the chat box takes focus). */
  releaseKeys() { this.keys.clear(); }

  private fireInput(event: string, code: string, inputType = 'Keyboard') {
    const rt = this.rt;
    if (!rt) return;
    const uis = this.dm.getService('UserInputService');
    if (!uis) return;
    const key = inputType === 'Keyboard' ? keyCodeName(code) ?? 'Unknown' : 'Unknown';
    rt.fire(uis, event, ['IO', ['E', 'KeyCode', key], ['E', 'UserInputType', inputType], ['V3', this.mouse.x, this.mouse.y, 0], ['E', 'UserInputState', event === 'InputBegan' ? 'Begin' : 'End']], false);
  }

  private onPointer(e: PointerEvent | WheelEvent, kind: 'down' | 'move' | 'up' | 'wheel') {
    const vp = this.vp;
    const r = vp.canvas.getBoundingClientRect();
    this.mouse = { x: e.clientX - r.left, y: e.clientY - r.top };
    if (kind === 'wheel') {
      const we = e as WheelEvent;
      const sp = this.dm.getService('StarterPlayer')!.props;
      this.camZoom = THREE.MathUtils.clamp(this.camZoom * (we.deltaY > 0 ? 1.15 : 0.87), sp.CameraMinZoomDistance as number, sp.CameraMaxZoomDistance as number);
      return;
    }
    const pe = e as PointerEvent;
    if (kind === 'down') {
      this.mouseButtons.add(pe.button);
      if (pe.button === 2) this.rmb = { x: pe.clientX, y: pe.clientY, moved: false };
      if (pe.button === 0) this.downAt = { x: pe.clientX, y: pe.clientY };
      if (this.firstPerson() && !this.pointerLocked) vp.canvas.requestPointerLock?.();
      this.fireInput('InputBegan', '', pe.button === 0 ? 'MouseButton1' : pe.button === 2 ? 'MouseButton2' : 'MouseButton3');
    } else if (kind === 'move') {
      if (this.rmb || this.pointerLocked) {
        this.camYaw -= pe.movementX * 0.006;
        this.camPitch = THREE.MathUtils.clamp(this.camPitch - pe.movementY * 0.006, -1.4, 1.4);
        if (this.rmb) this.rmb.moved = true;
      }
      if (!this.pointerLocked && pe.target === vp.canvas) vp.canvas.style.cursor = this.clickTarget(pe.clientX, pe.clientY) ? 'pointer' : '';
    } else if (kind === 'up') {
      this.mouseButtons.delete(pe.button);
      if (pe.button === 2) this.rmb = null;
      if (pe.button === 0 && this.downAt && Math.hypot(pe.clientX - this.downAt.x, pe.clientY - this.downAt.y) < 5) this.click(pe.clientX, pe.clientY);
      this.downAt = null;
      this.fireInput('InputEnded', '', pe.button === 0 ? 'MouseButton1' : pe.button === 2 ? 'MouseButton2' : 'MouseButton3');
    }
  }

  private clickTarget(x: number, y: number): Instance | null {
    const vp = this.vp;
    const r = vp.canvas.getBoundingClientRect();
    const hit = this.pointerLocked ? vp.pick(r.left + vp.canvas.clientWidth / 2, r.top + vp.canvas.clientHeight / 2) : vp.pick(x, y);
    if (!hit) return null;
    const cd = hit.inst.findFirstChildOfClass('ClickDetector');
    if (!cd || !this.character) return null;
    const dist = hit.point.distanceTo(this.character.headPosition);
    return dist <= (cd.props.MaxActivationDistance as number) ? cd : null;
  }

  private click(x: number, y: number) {
    const cd = this.clickTarget(x, y);
    if (!cd || !this.rt) return;
    // ClickDetector handlers are server scripts: on clients the click goes to the host
    if (this.isHost) this.rt.fire(cd, 'MouseClick', this.rt.encInst(this.player));
    else this.net?.send({ t: 'rpc', to: 'host', name: 'click', id: cd.id, args: [] });
  }

  private firstPerson() {
    const sp = this.dm.getService('StarterPlayer')!.props;
    return sp.CameraMode === 'LockFirstPerson' || this.camZoom <= 1;
  }

  // ---- frame ------------------------------------------------------------------------

  private frame = (dt: number) => {
    if (this.stopped) return;
    const ch = this.character;
    if (!this.paused) {
      this.time += dt;
      const k = this.keys;
      const f = (k.has('KeyW') || k.has('ArrowUp') ? 1 : 0) - (k.has('KeyS') || k.has('ArrowDown') ? 1 : 0);
      const s = (k.has('KeyD') || k.has('ArrowRight') ? 1 : 0) - (k.has('KeyA') || k.has('ArrowLeft') ? 1 : 0);
      const fwd = new THREE.Vector3(-Math.sin(this.camYaw), 0, -Math.cos(this.camYaw));
      const right = new THREE.Vector3(Math.cos(this.camYaw), 0, -Math.sin(this.camYaw));
      const move = fwd.multiplyScalar(f).add(right.multiplyScalar(s));
      if (move.lengthSq() > 1) move.normalize();
      if (ch && !ch.dead) ch.update(dt, move, k.has('Space'), this.firstPerson() ? this.camYaw : null);
      for (const p of this.puppets.values()) if (!p.dead) { p.updatePuppet(dt); if (this.role === 'host') p.detectTouches(); }

      this.physics.step(dt);
      if (this.isHost) this.physics.syncToInstances();
      if (ch && !ch.dead) {
        ch.postPhysics();
        if (ch.position.y < (this.dm.workspace.props.FallenPartsDestroyHeight as number)) ch.humanoid.set('Health', 0);
      }
      if (this.isHost) { this.updateAnims(dt); this.cleanupFallen(); }

      if (this.respawnTimer >= 0) {
        this.respawnTimer -= dt;
        if (this.respawnTimer < 0) this.spawnCharacter();
      }
      const rt = this.rt;
      if (rt) {
        const run = this.dm.getService('RunService');
        if (run) {
          rt.fire(run, 'Stepped', this.time, dt);
          rt.fire(run, 'RenderStepped', dt);
          rt.fire(run, 'Heartbeat', dt);
        }
        rt.step(dt, this.time);
      }
      if (this.net) {
        this.netTimer -= dt;
        if (this.netTimer <= 0) { this.netTimer = 0.05; this.sendState(); this.sendRep(); }
      }
    }
    this.updateCamera(dt);
    const cam = this.vp.camera;
    const dir = cam.getWorldDirection(new THREE.Vector3());
    this.audio.update(v3(cam.position.x, cam.position.y, cam.position.z), v3(dir.x, dir.y, dir.z));
    this.lbTimer -= dt;
    if (this.lbTimer <= 0) { this.lbTimer = 0.25; this.updateLeaderboard(); }
  };

  private cleanupFallen() {
    const limit = this.dm.workspace.props.FallenPartsDestroyHeight as number;
    for (const pb of [...this.physics.bodies.values()]) {
      if (pb.anchored || pb.body.translation().y > limit) continue;
      if (this.character && pb.inst.isDescendantOf(this.character.model)) continue;
      pb.inst.setParent(null);
    }
  }

  private updateCamera(dt: number) {
    const cam = this.vp.camera;
    const camInst = this.dm.workspace.findFirstChildOfClass('Camera');
    const fov = (camInst?.props.FieldOfView as number) ?? 70;
    if (cam.fov !== fov) { cam.fov = fov; cam.updateProjectionMatrix(); }
    if (camInst && camInst.props.CameraType !== 'Custom') {
      cam.position.copy(camInst.props.Position as THREE.Vector3Like);
      cam.quaternion.copy(orientationToQuat(camInst.props.Orientation as Vec3));
      return;
    }
    const ch = this.character;
    if (!ch) return;
    const target = ch.dead ? new THREE.Vector3().copy(ch.root.props.Position as THREE.Vector3Like).add(new THREE.Vector3(0, 1.5, 0)) : ch.headPosition;
    const fp = this.firstPerson();
    ch.setHidden(fp && !ch.dead);
    const dir = new THREE.Vector3(-Math.sin(this.camYaw) * Math.cos(this.camPitch), Math.sin(this.camPitch), -Math.cos(this.camYaw) * Math.cos(this.camPitch));
    if (fp) {
      cam.position.copy(target);
      cam.lookAt(target.clone().add(dir));
    } else {
      let dist = this.camZoom;
      const back = dir.clone().negate();
      const hit = this.physics.raycast(v3(target.x, target.y, target.z), v3(back.x * dist, back.y * dist, back.z * dist), new Set([ch.model]), 'Exclude', (c) => {
        const inst = this.physics.byCollider.get(c.handle);
        return !inst || !inst.id.startsWith('char-'); // never pull the camera in for other players
      });
      if (hit) dist = Math.max(0.5, hit.distance - 0.6);
      const desired = target.clone().addScaledVector(back, dist);
      cam.position.lerp(desired, Math.min(1, dt * 20 + (cam.position.distanceTo(desired) > 30 ? 1 : 0)));
      cam.lookAt(target);
    }
    if (camInst) {
      camInst.setSilent('Position', v3(cam.position.x, cam.position.y, cam.position.z));
      camInst.setSilent('Orientation', quatToOrientation(cam.quaternion));
    }
  }

  // ---- leaderboard -------------------------------------------------------------------

  private buildLeaderboard() {
    this.leaderboard = document.createElement('div');
    this.leaderboard.className = 'leaderboard';
    this.vp.container.appendChild(this.leaderboard);
  }

  private updateLeaderboard() {
    const lb = this.leaderboard;
    if (!lb) return;
    const rows = this.roster();
    const cols = [...new Set(rows.flatMap((r) => r.stats.map((s) => s[0])))];
    const show = cols.length > 0 || rows.length > 1;
    lb.style.display = show ? 'block' : 'none';
    if (!show) return;
    const fmt = (v: unknown) => (typeof v === 'number' ? (Number.isInteger(v) ? String(v) : v.toFixed(1)) : v === undefined ? '' : String(v));
    const html = `<table><tr><th>${esc(t('Player'))}</th>${cols.map((c) => `<th>${esc(c)}</th>`).join('')}</tr>${rows.map((r) =>
      `<tr class="${r.me ? 'me' : ''}"><td>${esc(r.name)}</td>${cols.map((c) => `<td>${esc(fmt(r.stats.find((s) => s[0] === c)?.[1]))}</td>`).join('')}</tr>`).join('')}</table>`;
    if (lb.innerHTML !== html) lb.innerHTML = html;
  }

  // ---- control -----------------------------------------------------------------------

  exec(code: string) { if (this.rt) this.rt.exec(code); else this.log('warn', t('Scripts are still loading…')); }

  pause() { this.paused = true; this.audio.setPaused(true); this.log('info', t('Test paused.')); this.changed.fire(); }
  resume() { this.paused = false; this.audio.setPaused(false); this.log('info', t('Test resumed.')); this.changed.fire(); }

  /** Stops the session. `closeNet` = also disconnect from the server. */
  stop(closeNet = true) {
    if (this.stopped) return;
    this.stopped = true;
    if (this.keyHandlers) {
      window.removeEventListener('keydown', this.keyHandlers.down);
      window.removeEventListener('keyup', this.keyHandlers.up);
      window.removeEventListener('blur', this.keyHandlers.blur);
    }
    if (document.pointerLockElement) document.exitPointerLock();
    this.unsub.forEach((u) => u());
    this.unsub = [];
    this.replicator?.dispose();
    this.audio.dispose();
    this.audio.setPaused(false);
    this.rt?.dispose();
    this.physics?.dispose();
    this.leaderboard?.remove();
    this.vp.canvas.style.cursor = '';
    this.vp.gui.onButtonClick = () => {};
    this.vp.gui.onMouseEnter = () => {};
    if (closeNet) this.net?.close();
    if (this.role === 'local') {
      this.host.output.system(t('Test stopped after {s} s{errors}. The scene was restored.', {
        s: this.time.toFixed(1), errors: this.errors ? t(' with {n} error(s)', { n: this.errors }) : '',
      }));
    }
  }
}

// ---- helpers ------------------------------------------------------------------------------

function r3(x: number) { return Math.round(x * 1000) / 1000; }
function esc(s: string) { return s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]!)); }

/** Encoded Lua value -> plain JSON (DataStore values: nil, booleans, numbers, strings, tables). */
export function luaToJson(v: unknown): unknown {
  if (v === undefined || v === null || typeof v === 'boolean' || typeof v === 'number' || typeof v === 'string') return v ?? null;
  if (Array.isArray(v)) {
    if (v[0] === 'N') return null;
    if (v[0] === 'T') return v.slice(1).map(luaToJson);
    if (v[0] === 'D') { const o: Record<string, unknown> = {}; for (let i = 1; i < v.length; i += 2) o[String(v[i])] = luaToJson(v[i + 1]); return o; }
    throw new Error(`DataStores cannot store ${v[0] === 'I' ? 'Instances' : v[0] === 'V3' ? 'Vector3 values' : 'this type'} – use numbers, strings, booleans and tables`);
  }
  throw new Error('DataStores cannot store this type');
}

/** Plain JSON -> encoded Lua value. */
export function jsonToLua(v: unknown): Enc {
  if (v === null || v === undefined) return ['N'];
  if (Array.isArray(v)) return ['T', ...v.map(jsonToLua)];
  if (typeof v === 'object') { const out: unknown[] = ['D']; for (const [k, x] of Object.entries(v as object)) out.push(k, jsonToLua(x)); return out; }
  return v as Enc;
}

/** Studio play tests: DataStores persist per project in local storage. */
function localDataStore(project: string, store: string, key: string) {
  const k = `forge.ds.${project}.${store}`;
  const read = (): Record<string, unknown> => { try { return JSON.parse(localStorage.getItem(k) ?? '{}'); } catch { return {}; } };
  const write = (m: Record<string, unknown>) => { try { localStorage.setItem(k, JSON.stringify(m)); } catch { throw new Error('DataStore storage is full'); } };
  return {
    get: async () => read()[key] ?? null,
    set: async (v: unknown) => { const m = read(); m[key] = v; write(m); },
    remove: async () => { const m = read(); delete m[key]; write(m); },
  };
}

Studio.createPlaySession = (studio, doc) => new PlaySession(
  { viewport: studio.viewport!, output: studio.output, notify: () => studio.emit() },
  { scene: serializeScene(doc.dm), assets: doc.assets, name: doc.name, key: doc.libraryKey ?? doc.name },
);

# Forge Studio – Architecture

Three programs share one code base:

```
 Forge Studio (Electron, dev)        Forge Server (Rust, axum/tokio)          Forge Player (Electron)
 ┌───────────────────────┐  publish  ┌─────────────────────────────┐  HTTP  ┌──────────────────────┐
 │ editor, local tests,  │ ────────▶ │ games / versions / live     │ ◀───── │ game browser         │
 │ project library       │  (key)    │ content-addressed objects   │        │ PlaySession (host or │
 └───────────────────────┘           │ sessions (WebSocket relay)  │ ◀────▶ │ client), chat, HUD   │
                                     │ DataStores                  │   WS   └──────────────────────┘
                                     └─────────────────────────────┘
```

## Technology stack (and why)

| Concern | Choice | Reason |
|---|---|---|
| Desktop shell | **Electron** | Mature, cross-platform, full Chromium (WebGL2, WebAssembly, workers), native file dialogs. Tauri would need a Rust toolchain and system WebViews with less predictable WebGL support. |
| UI | **React 18 + TypeScript** | Component model for dockable panels, strong typing across the engine. |
| 3D | **Three.js** | Proven WebGL renderer with shadows, PBR materials, glTF import/export, TransformControls gizmo. |
| Native core | **Rust → WebAssembly** (`engine/`, crate `forge-core`) | Terrain storage, sculpting brushes, procedural generation, chunked meshing and terrain ray queries run as compiled Rust in the same process – no IPC latency, near-native speed. Plain C ABI (no bindgen), 39 KB module. |
| Physics | **Rapier** (Rust, compiled to WASM) | Production-grade engine: rigid bodies, CCD, sensors, heightfields, collision events and a kinematic character controller (slopes, steps, ground snapping). |
| Scripting | **Lua 5.4 via wasmoon** (WebAssembly) | Real Lua VM, sandboxable, coroutines for `wait()`. Luau is not available as a maintained WASM build; we document the differences instead of claiming compatibility. |
| Code editor | **Monaco** | Same editor as VS Code: highlighting, find/replace, markers, completion API. |
| Storage | Versioned JSON project file (`.forge`) | One portable file containing scene, scripts, settings and embedded assets (base64). |

## Module layout (`src/`)

```
core/        Object system – no UI or rendering dependencies
  Instance.ts      Instance + DataModel (hierarchy, change signals, id index)
  classes.ts       Class registry: properties, defaults, events, methods, icons
  History.ts       Undo/redo (transactions of property/parent/attribute/terrain ops)
  Serializer.ts    Versioned file formats, validation, migration
  types.ts         Vec3 / UDim2 / Color helpers
engine/      (repo root) Rust crate forge-core → src/engine/forge_core.wasm
src/engine/  WASM loader + typed memory views for the Rust core
terrain/     TerrainData wrapper around the Rust core, brush/generator options
assets/      Asset store (project assets), built-in library, prefabs, synthesized media
render/      Three.js viewport: scene sync, GPU instancing (Batcher), materials,
             environment, gizmos, particles, chunked terrain/water meshes, GUI overlay,
             glTF loading, thumbnails
runtime/     Play mode: PlaySession, Lua runtime (JS bridge + Lua prelude),
             physics, character controller, audio, enums
editor/      Studio controller (documents, commands, clipboard, insert, play,
             autosave, shortcuts), Document, Selection, Output log, self-test
project/     File I/O abstraction (Electron IPC / File System Access API),
             autosave storage, templates & example projects
ui/          React: App shell, menu bar, ribbon, dock layout, panels
             (Toolbox, Explorer, Properties, Output, Assets, Terrain, Script Analysis),
             Monaco setup + script editor, dialogs, start page
net/         ServerApi (REST), NetClient (WebSocket), Replicator (world sync)
player/      Forge Player UI (game browser, loading, HUD, chat, console)
i18n/        t()/msg(), built-in English + German, language packs
electron/    main.cjs (studio/player mode, splash, app:// protocol, CSP, IPC,
             project library, local server), preload.cjs, splash.html
server/      (repo root) Rust crate forge-server
```

Dependency direction: `ui → editor → (render, runtime, project, assets) → core`.
`core` never imports UI or Three.js (except type-only terrain data).

## Data flow

1. **Every mutation goes through `Instance.set` / `Instance.setParent`.** Attached
   instances fire `DataModel` signals (`added`, `removed`, `reparented`, `changed`,
   `terrainChanged`) and report a `HistoryOp` to the document's `History`.
2. The **Viewport** subscribes to those signals and keeps one Three.js object per
   part/light/emitter/camera/terrain – no polling, no full rebuilds.
3. **Explorer / Properties** subscribe as well (throttled to one render per frame;
   Explorer ignores non-structural changes so physics updates don't re-render it).
4. **Undo/redo** replays ops with recording suspended; transactions group ops
   (`doc.transact(label, fn)`), consecutive script edits are coalesced.

## Play mode

`Studio.startPlay()` creates a `PlaySession`:

1. Serializes the edit scene and deserializes an **isolated copy** (ids preserved so
   runtime errors link back to the edit scripts). The project is never modified.
2. Creates the Player, PlayerGui (copied StarterGui) and PlayerScripts.
3. Builds Rapier bodies (anchored = fixed, switched to kinematic when scripts move
   them; CanCollide=false = sensor; terrain = heightfield).
4. Starts a Lua state, runs `Script`s (Workspace / ServerScriptService) and
   `LocalScript`s (player containers), fires `PlayerAdded`, spawns the character.
5. Each frame: input → character controller → physics step (fixed 60 Hz) →
   batched transform sync (`DataModel.transformsChanged`, one event per frame) →
   animations → RunService events → Lua scheduler (`__step`) → camera → audio.
6. `Stop` disposes everything and re-binds the viewport to the edit scene.

### Lua bridge

Lua sees instances as proxies (`setmetatable` with `__index/__newindex`). All
JS↔Lua traffic uses primitives and tagged arrays (`{"V3",x,y,z}`, `{"I",id,class}`,
`{"CF",...}`), so no JS objects leak into Lua. Events are queued in JS and
delivered in **one** call per frame (JS→Lua calls are comparatively expensive).
Datatypes (Vector3, CFrame, Color3, UDim2, Enum), signals, the coroutine scheduler,
TweenService and Debris are implemented in Lua (`runtime/luaPrelude.ts`).
A per-coroutine instruction hook aborts scripts that run > 5 s without yielding.

## Server: git-like publishing

`server/src/store.rs` stores every published project once, content-addressed by
SHA-256 (`objects/<hash>`), and per game an append-only list of versions
`{number, parent, hash, message, author, created, size}` plus the *live*
version. Publishing sends the version the project was based on as `parent`; if
another version was published in the meantime the server answers **409
Conflict** (like a rejected push) unless `force` is set. Rollback = making an
older version live. All writes are atomic (temp file + rename). Publishing and
game edits need the publish key (`x-forge-key`).

REST: `GET /api/games`, `POST /api/games`, `GET|PATCH /api/games/:id`,
`POST /api/games/:id/versions`, `GET /api/games/:id/versions/:n`,
`GET|POST /api/games/:id/live`, `GET|PUT|DELETE /api/games/:id/datastore/:store/:key`,
WebSocket `/ws/:id?name=&version=`.

## Multiplayer

`server/src/realtime.rs` keeps one session per (game, version), up to 32
players. The server is a validating relay: it assigns ids, elects the **host**
(first player; re-elected when the host leaves), rate-limits character state,
accepts world replication only from the host, routes RemoteEvent RPCs (to host,
to one player, to all) and authorizes DataStore writes by the host's session
token.

In Forge Player, `PlaySession` runs in one of three roles:

| Role | Scripts | Physics | Sends |
|---|---|---|---|
| local (Studio test) | Script + LocalScript | full | – |
| host | Script + own LocalScripts | full, authoritative | world ops 20×/s, snapshots for joiners, own character |
| client | own LocalScripts | kinematic copy of host bodies, own character | own character, RemoteEvent calls |

`net/Replicator.ts` turns DataModel signals into compact ops (create, destroy,
move, property, attribute, transforms). Character parts and per-player
containers are excluded; ids are deterministic (`player-<id>`, `char-<id>-…`)
so all peers address the same objects. Remote characters are *puppets*
interpolated from state messages. On host migration the new host restarts the
world from the published version and broadcasts a snapshot. The viewport keeps
stepping the simulation from a timer while the window is hidden, and Electron
windows disable background throttling, so a minimized host does not freeze the
game.

## Internationalization

UI texts are written in English and wrapped in `t('…', {vars})` (rendered now)
or `msg('…')` (marked for translation, translated where displayed). The English
text is the key; `src/i18n/de.ts` holds the German pack. `npm run i18n:check`
lists missing/unused keys (it also extracts strings from data tables such as the
class registry). Users add languages as JSON packs, see
[LANGUAGE_PACKS.md](LANGUAGE_PACKS.md).

## Project format (version 1)

```jsonc
{
  "format": "forge-project", "version": 1,
  "name": "...", "created": "ISO date", "modified": "ISO date",
  "settings": { "gridSize": 4, "moveSnap": 1, "rotateSnap": 15, "snapEnabled": true, "description": "" },
  "assets": [{ "id", "name", "kind": "model|texture|sound|animation|script|material", "mime", "data", "encoding": "base64|text" }],
  "scene": { "services": [ InstanceJSON ] },
  "editor": { "camera": { "position": [x,y,z], "yaw": 0, "pitch": 0 } }
}
InstanceJSON = { "c": className, "id": string, "p": { prop: value }, "a"?: attributes, "t"?: terrain, "ch"?: [InstanceJSON] }
```

Also `forge-scene` (scene + assets) and `forge-model` (instances + used assets).
Loading validates every property against the class registry; unknown classes
become Folders and invalid values fall back to defaults – each case is reported in
the Output. Files from newer format versions are rejected with a clear message;
`migrate()` in `Serializer.ts` is the hook for future upgrades. Saves in Electron
are atomic (write temp file + rename).

## Extending

- **New instance class:** add a `ClassDef` in `core/classes.ts` (props, events,
  methods). Properties panel, serializer, Insert menus, autocomplete and the Lua
  bridge pick it up automatically. Add rendering in `render/Viewport.ts` and
  behaviour in `runtime/PlaySession.ts` (`hostCall`) if needed.
- **New script API:** JS methods go in `LuaRuntime.bridgeCall` or the play
  session `host`; pure-Lua helpers in `luaPrelude.ts`.
- **New panel:** add a component to `ui/panels`, register it in `App.tsx` and
  `DEFAULT_LAYOUT` (Dock.tsx).

## Development phases (status)

| Phase | Content | Status |
|---|---|---|
| 1 | App shell, Electron window, menu, ribbon, dock layout | done |
| 2 | Viewport, editor camera, grid, parts, materials, lighting, sky | done |
| 3 | Explorer, Properties, viewport sync | done |
| 4 | Selection, gizmos, scale handles, drag-move, snapping, undo/redo, clipboard, grouping | done |
| 5 | Project save/open/save-as, scene/model/GLB export, autosave + recovery, examples | done |
| 6 | Monaco editor, Lua runtime, API, errors, formatter, script analysis | done |
| 7 | Play/pause/stop/restart, character, physics, camera, GUI, debug command bar | done |
| 8 | Terrain editor, asset browser, import/export, sounds, particles, animations | done |
| 9 | Theme, responsive dock sizes, CSP, performance (shared geometry/materials, virtualized Explorer) | done |
| 10 | Built-in self-test (`Test ▸ Run Self-Test`) and Electron smoke test (`npm run smoke`) | done |
| 11 | Rust server, publishing/versions, Forge Player, multiplayer, DataStores | done |
| 12 | Start screen + project library, splash, languages, media import, UI editor with blur effects | done |

## Performance design

- **Rendering:** opaque parts are drawn through `render/Batcher.ts` – one
  `InstancedMesh` per geometry/material/shadow combination (3000 parts ≈ 26
  draw calls). Transparent parts keep individual meshes for correct sorting.
  The triplanar material shader reads the per-instance scale.
- **Physics sync:** the simulation writes positions directly and emits a single
  `transformsChanged` batch per frame instead of per-property events.
- **Terrain:** meshing happens in Rust per 32×32-cell chunk; brush strokes only
  re-mesh touched chunks; editor picking ray-marches the heightmap in Rust
  instead of testing triangles. Undo stores only the touched rectangle.
- **Scripts:** JS→Lua calls are batched (one `__step` per frame with all queued events).

Benchmark (`npm run build && npx electron . --bench`, 3000 parts of which 1000
unanchored, Windows 11 desktop): editor ≈ 8.6 ms/frame (p95 10.4 ms, 26 draw
calls); play mode ≈ 19.8 ms/frame with Rapier step ≈ 4.5 ms and transform sync
≈ 1.5 ms. Numbers depend on the GPU/CPU.

## Building the Rust core

`npm run engine` compiles `engine/` with `cargo build --release --target
wasm32-unknown-unknown` and copies the module to `src/engine/forge_core.wasm`
(also run by `npm run build`/`npm run dev`). Without a Rust toolchain the
committed prebuilt module is used. `npm run engine:test` also runs the Rust
unit tests natively. Toolchain: `rustup` + `rustup target add wasm32-unknown-unknown`.

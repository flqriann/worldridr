# WorldRidr Studio

World editor for the driving simulator **worldridr**, built on the Forge Studio code base
(Electron + React + Three.js, Rust/WASM terrain, Rapier physics, Lua 5.4 scripting, Roblox-Studio-style workflow).

```bash
npm install
npm run app:dev      # editor with hot reload (desktop app)
npm run app          # production build + desktop app
npm run typecheck
```

## What it does

- **Import a place** (*File → Import Place*, or the **Import** button): type a town, address or `lat, lon`.
  Terrain comes from real elevation data (AWS Terrain Tiles), buildings/roads/water from OpenStreetMap.
  Everything becomes ordinary, editable objects (radius 150–2000 m, default 500 m).
- **Smooth rendering**: buildings, roads and map meshes are baked into 192 m chunks (a few dozen draw calls for thousands of
  objects), terrain is meshed in Rust per chunk, the Explorer is virtualized.
- **Edit everything like in Roblox Studio**: select/move/rotate/scale with gizmos, undo/redo, Explorer + Properties, terrain brushes,
  parts, models, lights, GUI. Buildings (footprint, height, roof shape/colors) are `Building` objects, roads are `Road` objects.
- **Road tool** (ribbon *World → Road*): click points on the ground, Enter / double-click finishes, Backspace removes the last point.
  Select a road to get handles: drag = move point, Alt+click = delete point, Ctrl+click on the road = insert point.
  Roads follow the terrain; the type (Stadtstraße, Landstraße, 4 Spuren, Einbahn, Autobahn, Feldweg) matches the game.
- **Scripts**: Lua (Toolbox → Gameplay → *Shop Zone*, *Fuel Station Zone*, *Teleporter*, *Message Zone*). Press **Play (F5)** to walk
  around and test them. Scripts are exported next to the map.

## Shared map with git (the whole world together)

The base world is **OpenStreetMap** – the game already streams roads, buildings, water and terrain for the entire planet.
A map repository therefore stores only what people **add or change**, which keeps it small and merge-friendly.

*File → Map (Git)*:

| Command | What it does |
| --- | --- |
| Clone Map Repository… | `git clone` a public map repo and open it |
| Save Map | writes `worldridr-map.json`, `tiles/z15_x_y.json` (one object/road per line, sorted by id), optional `terrain.json`, `scripts/` |
| Commit & Push… | saves, commits (asks for a message), adds the remote on first use, pushes |
| Pull Latest Changes | pulls and reopens the map |
| Include imported OpenStreetMap objects | off by default (OSM is the base); turn on to snapshot imported buildings/roads too |

Different areas live in different tile files, so contributors rarely conflict. On a conflict inside a tile file keep both lines
(each object has a unique id), remove the markers and commit. Start the game with `WR_MAPDIR=<map folder>` or put the folder at `maps/world`.
The format is the same one the game reads (`src/mapformat.cpp`); buildings are exported as `mesh` objects with an extra `building` block so
they come back as editable buildings.

Needs `git` installed and credentials for the remote (e.g. Git Credential Manager).

## Coordinates and units

1 unit = 1 meter. `x` = east, `z` = south, `y` = up, origin = `Workspace.GeoLat/GeoLon`, `Y = 0` = `Workspace.GeoElevation` above sea level.
Gravity is 9.81. The projection is identical to the game's.

## Known limits

- The editor does not change how the game treats OSM: imported OSM buildings are not exported by default, and the game cannot hide
  the original OSM building where you edited it (needs a game-side override feature).
- Lua scripts use the Forge API (Roblox-like) and run in the editor's Play mode; the game's own script engine does not execute them yet.
- `MeshPart`/glb models are not part of the map format (exported as blocks). Wedge orientation and cylinder axis follow the game's
  conventions as best as they could be derived from the source; verify in the game.
- Not yet tested against a running game build: map files were round-trip tested inside the editor only.

Forge Server/Player code from Forge Studio is still in the tree but hidden from the menus.

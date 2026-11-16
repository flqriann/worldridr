# Forge Studio – Scripting API

Scripts are written in **Lua 5.4** (running in WebAssembly via wasmoon) with a
Roblox-like object API. It is **not** a Luau implementation:

| Supported | Not supported |
|---|---|
| Standard Lua 5.4 syntax and libraries (`math`, `string`, `table`, `coroutine`, `os.clock/time/date`) | Luau type annotations (`local x: number`) |
| Luau-style compound assignment `x += 1`, `-=`, `*=`, `/=`, `..=` (rewritten before compiling, one statement per line) | `continue`, string interpolation `` `{x}` ``, generalized iteration without `pairs` |
| `task`, `wait`, `delay`, `spawn`, `typeof`, `require` | HttpService, MessagingService, BindableEvents |
| RemoteEvents and DataStores (multiplayer, see below) | |
| | `io`, `os.execute`, file access (removed for safety) |

## Where scripts run

| Class | Runs when the test starts if it is inside |
|---|---|
| `Script` | `Workspace` or `ServerScriptService` |
| `LocalScript` | `StarterPlayer.StarterPlayerScripts`, `StarterGui` (copied into `PlayerGui`) or the character |
| `ModuleScript` | only via `require(module)`; the module must return exactly one value and may not yield |

In a Studio test there is one Lua state that runs both kinds of scripts. In
multiplayer (Forge Player) see [Multiplayer](#multiplayer). `Enabled = false` stops a
script; destroying a script stops its threads and connections. A script that
runs longer than **5 seconds without yielding** is stopped with a timeout error.
The Output panel's command bar runs Lua inside the running game, or – when no
test runs – against the edit scene as one undoable step.

## Globals

`game`, `workspace`/`Workspace`, `script`, `Instance`, `Vector3`, `Vector2`,
`Color3`, `CFrame`, `UDim`, `UDim2`, `Enum`, `TweenInfo`, `RaycastParams`,
`task`, `wait(t)`, `delay(t, f)`, `spawn(f)`, `tick()`, `time()`, `typeof(v)`,
`require(m)`, `print(...)`, `warn(...)`, `shared`, `_G` (shared between scripts).

### task
`task.wait(t?) -> elapsed` · `task.spawn(f, ...)` · `task.defer(f, ...)` ·
`task.delay(t, f, ...)` · `task.cancel(thread)`

## Instances

```lua
local part = Instance.new("Part")      -- or Instance.new("Part", parent)
part.Size = Vector3.new(4, 1, 2)
part.Position = Vector3.new(0, 10, 0)
part.Color = Color3.fromRGB(255, 80, 80)
part.Material = Enum.Material.Neon    -- strings like "Neon" are accepted too
part.Anchored = true
part.Parent = workspace
```

Children can be read by name (`workspace.Baseplate`). Common members of every
instance: `Name`, `ClassName`, `Parent`, `Destroy()`, `Clone()`,
`FindFirstChild(name, recursive?)`, `FindFirstChildOfClass`,
`FindFirstChildWhichIsA`, `FindFirstAncestor`, `FindFirstAncestorOfClass`,
`WaitForChild(name, timeout?)`, `GetChildren()`, `GetDescendants()`, `IsA`,
`IsDescendantOf`, `GetFullName()`, `ClearAllChildren()`,
`GetAttribute/SetAttribute`, `GetPropertyChangedSignal(prop)`, events
`Changed`, `ChildAdded`, `ChildRemoved`, `AncestryChanged`, `Destroying`.

### Creatable classes
`Part` (Shape: Block/Ball/Cylinder), `WedgePart`, `SpawnLocation`, `MeshPart`,
`Model`, `Folder`, `Script`, `LocalScript`, `ModuleScript`, `PointLight`,
`SpotLight`, `ParticleEmitter`, `Sound`, `KeyframeAnimation`, `ClickDetector`,
`Camera`, `Sky`, `Atmosphere`, `ScreenGui`, `Frame`, `TextLabel`, `TextButton`,
`ImageLabel`, `BoolValue`, `IntValue`, `NumberValue`, `StringValue`,
`Vector3Value`, `Color3Value`. All properties are listed in the Properties
panel with tooltips.

### BasePart
Properties: `Position`, `Orientation` (degrees, Y-X-Z order), `CFrame`, `Size`,
`Color`, `Material`, `Transparency`, `Reflectance`, `TextureId`, `Visible`,
`CastShadow`, `Anchored`, `CanCollide`, `CanTouch`, `Locked`, `Density`,
`Friction`, `Elasticity`, `AssemblyLinearVelocity` (alias `Velocity`).
Methods: `GetMass()`, `ApplyImpulse(v)`, `GetTouchingParts()`.
Events: `Touched(otherPart)`, `TouchEnded(otherPart)`.

### Model
`GetPivot()`, `PivotTo(cframe)`, `MoveTo(position)`, `GetBoundingBox() -> cframe, size`,
`PrimaryPart` (set by script).

### Services (`game:GetService(name)`)
- **Workspace** – `Gravity`, `FallenPartsDestroyHeight`, `CurrentCamera`,
  `Raycast(origin, direction, params?) -> RaycastResult | nil`
  (`Instance`, `Position`, `Normal`, `Material`, `Distance`),
  `GetPartsInRadius(position, radius)`.
- **Players** – `LocalPlayer`, `GetPlayers()`, `GetPlayerFromCharacter(model)`,
  events `PlayerAdded`, `PlayerRemoving`. A `leaderstats` folder with value
  objects inside a player is shown as a leaderboard.
- **Player** – `Character`, `RespawnLocation` (a SpawnLocation/part),
  `LoadCharacter()`, events `CharacterAdded`, `CharacterRemoving`.
- **Humanoid** (in the character) – `Health`, `MaxHealth`, `WalkSpeed`,
  `JumpPower`, `MoveDirection`, `FloorMaterial`, `TakeDamage(n)`, `Jump()`,
  events `Died`, `HealthChanged`, `Jumping`. The character contains
  `HumanoidRootPart`, `Torso`, `Head`, arms and legs.
- **RunService** – `Heartbeat(dt)`, `Stepped(time, dt)`, `RenderStepped(dt)`.
- **UserInputService** – `InputBegan(input, gameProcessed)`, `InputEnded`,
  `IsKeyDown(Enum.KeyCode.X)`, `IsMouseButtonPressed(Enum.UserInputType.MouseButton1)`,
  `GetMouseLocation()`. `input.KeyCode`, `input.UserInputType`, `input.Position`.
- **TweenService** – `TweenService:Create(instance, TweenInfo.new(time, style, direction, repeatCount, reverses, delay), goals)`
  returns a tween with `Play()`, `Pause()`, `Cancel()`, `Completed`. Tweens
  numbers, Vector3, Color3, UDim2 and CFrame properties.
- **Debris** – `AddItem(instance, seconds)`.
- **Lighting** – `ClockTime`, `Brightness`, `Ambient`, `OutdoorAmbient`,
  `FogColor/FogStart/FogEnd`, `GlobalShadows`, `ExposureCompensation`.
- **SoundService** – `Volume`.

### Other classes
- **Sound** – `SoundId`, `Volume`, `PlaybackSpeed`, `Looped`, `Playing`,
  `RollOffMaxDistance`; `Play()`, `Stop()`, `Pause()`, `Resume()`; `Ended`, `Played`.
  Positional when parented to a part.
- **ParticleEmitter** – emission/appearance properties, `Emit(count)`, `Clear()`.
- **KeyframeAnimation** – `AnimationId`, `AutoPlay`, `Looped`, `Speed`,
  `Play()`, `Stop()`, `Stopped`. Animation assets are JSON:
  `{ "duration": 2, "easing": "smooth", "keyframes": [{ "t": 0, "position": [0,0,0], "rotation": [0,0,0], "scale": 1 }] }`
  (offsets relative to the part/model when the animation starts).
- **ClickDetector** – `MaxActivationDistance`, event `MouseClick(player)`.
- **Camera** – the first Camera in Workspace is `workspace.CurrentCamera`.
  `CameraType = "Custom"` follows the player; `"Scriptable"`/`"Fixed"` render
  from the camera's `CFrame`/`Position`/`Orientation`. `FieldOfView`.
- **GUI** – `ScreenGui` (`Enabled`, `DisplayOrder`), `Frame`, `TextLabel`,
  `TextButton` (`MouseButton1Click`), `ImageLabel`; `Position`/`Size` are
  `UDim2`, plus `AnchorPointX/Y`, `BackgroundColor3`, `BackgroundTransparency`,
  `CornerRadius`, `Text`, `TextColor3`, `TextSize`, `TextScaled`, `Font`.
  Effects (Forge extensions, category *Effects* in Properties):
  `BackgroundBlur` (px, blurs the 3D view behind a semi-transparent element),
  `GradientEnabled`, `GradientColor`, `GradientRotation`, `StrokeThickness`,
  `StrokeColor`, `StrokeTransparency`, `ShadowSize`, `ShadowTransparency`.
- **Decal** – image on one face of its parent part: `Texture` (image asset),
  `Face` (`Front`/`Back`/`Top`/`Bottom`/`Left`/`Right`), `Color3`, `Transparency`.

## Multiplayer

Published games are played in **Forge Player**. The first player on a server is
the **host**: their Player runs all `Script`s and the physics simulation, and its
world is replicated to everyone else (about 20 updates per second). Every player
runs their own `LocalScript`s, character and camera. In a Studio test you are
host and the only player.

- `game.Players` contains all connected players; `PlayerAdded` / `PlayerRemoving`
  fire on the host for every player. `Players.LocalPlayer` is you.
- Other players' characters are shown and animated; their `Humanoid.Health`,
  `WalkSpeed` and `JumpPower` are replicated.
- If the host leaves, another player becomes host and the world restarts from the
  published version (DataStore data is kept).

### RemoteEvent
Create it in `ReplicatedStorage` (Toolbox ▸ Gameplay ▸ Remote Event).

| Call | From | Effect |
|---|---|---|
| `ev:FireServer(...)` | LocalScript | `ev.OnServerEvent(player, ...)` on the host |
| `ev:FireClient(player, ...)` | Script | `ev.OnClientEvent(...)` on that player |
| `ev:FireAllClients(...)` | Script | `ev.OnClientEvent(...)` on every player |

Arguments: numbers, strings, booleans, tables, Vector3/Color3/UDim2/CFrame and
instances that exist on both sides.

### DataStoreService
Persistent data per game, stored on the Forge Server (in a Studio test without
a server: in local storage, per project). Only server `Script`s may use it.
Calls yield until the server has answered.

```lua
local store = game:GetService("DataStoreService"):GetDataStore("Coins")
game.Players.PlayerAdded:Connect(function(player)
	local coins = store:GetAsync("p_" .. player.Name) or 0
	store:SetAsync("p_" .. player.Name, coins + 10)
end)
```

`GetAsync(key)`, `SetAsync(key, value)`, `IncrementAsync(key, delta?) -> number`,
`RemoveAsync(key)`, `UpdateAsync(key, function(old) return new end)`. Keys:
1–64 letters, digits, `- _ .` or spaces; values: numbers, strings, booleans and
tables of those (max. 256 KB per key).

## Datatypes
- **Vector3** – `new`, `zero`, `one`, `xAxis/yAxis/zAxis`; `X Y Z`, `Magnitude`,
  `Unit`, `Dot`, `Cross`, `Lerp`, `FuzzyEq`, `Abs`, `Floor`, `Min`, `Max`; `+ - * /` and unary minus.
- **Vector2** – `new`, `X Y`, `Magnitude`, `Unit`, `Dot`, `Lerp`.
- **Color3** – `new(r,g,b)` (0–1), `fromRGB`, `fromHSV`, `fromHex`; `Lerp`, `ToHSV`, `ToHex`.
- **CFrame** – `new(x,y,z)`, `new(pos)`, `new(pos, lookAt)`, `lookAt`,
  `Angles(rx,ry,rz)` (= `fromEulerAnglesXYZ`), `fromEulerAnglesYXZ`
  (= `fromOrientation`), `fromAxisAngle`; `Position`, `LookVector`,
  `RightVector`, `UpVector`, `Rotation`; `*` (CFrame or Vector3), `+`/`-` Vector3,
  `Inverse`, `Lerp`, `ToEulerAnglesYXZ`, `ToEulerAnglesXYZ`, `ToObjectSpace`,
  `ToWorldSpace`, `PointToWorldSpace`, `PointToObjectSpace`, `GetComponents`.
- **UDim / UDim2** – `UDim2.new(xs, xo, ys, yo)`, `fromScale`, `fromOffset`, `Lerp`.
- **Enum** – `Material`, `PartType`, `KeyCode`, `UserInputType`,
  `UserInputState`, `EasingStyle`, `EasingDirection`, `PlaybackState`,
  `CameraType`, `CameraMode`, `NormalId`, `Font`, `TextXAlignment`,
  `ParticleShape`, `RaycastFilterType`. `item.Name`, `item.Value`, `Enum.X:GetEnumItems()`.

## Example

```lua
local Players = game:GetService("Players")
local TweenService = game:GetService("TweenService")

Players.PlayerAdded:Connect(function(player)
	local stats = Instance.new("Folder")
	stats.Name = "leaderstats"
	stats.Parent = player
	local points = Instance.new("IntValue", stats)
	points.Name = "Points"
end)

local pad = workspace:WaitForChild("Pad")
pad.Touched:Connect(function(hit)
	local player = Players:GetPlayerFromCharacter(hit.Parent)
	if player then
		player.leaderstats.Points.Value += 1
		TweenService:Create(pad, TweenInfo.new(0.3), { Color = Color3.new(0, 1, 0) }):Play()
	end
end)
```

## Known limitations
- Multiplayer is host-authoritative: server Scripts run in the host's Forge
  Player, not inside the Rust server (see Multiplayer).
- Physics: Rapier rigid bodies (Rust/WASM); no constraints/joints/welds yet –
  unanchored parts inside a model fall independently. Like Roblox, parts moved
  by the physics simulation do not fire `Changed`.
- `Humanoid:MoveTo`, NPC humanoids and `Player:GetMouse()` are not supported.
- MeshPart collision uses the bounding box.

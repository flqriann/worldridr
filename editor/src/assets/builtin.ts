// Built-in, read-only asset library ("Toolbox"). Ids start with "builtin:".
// Media is generated procedurally on first access (see synth.ts).

import type { AssetRecord } from '../core/Serializer';
import { MATERIALS } from '../core/classes';
import { SYNTH_SOUNDS, SYNTH_TEXTURES, renderTexture } from './synth';

export const PREFAB_NAMES = [
  'Tree', 'Pine Tree', 'House', 'Lamp Post', 'Staircase', 'Bridge', 'Coin', 'Kill Brick', 'Jump Pad',
  'Moving Platform', 'Campfire', 'Spinner', 'Door', 'Checkpoint Flag', 'Crate Stack',
] as const;

export const SCRIPT_TEMPLATES: Record<string, string> = {
  'Kill Brick': `-- Kills any character that touches the parent part.
local part = script.Parent

part.Touched:Connect(function(hit)
\tlocal humanoid = hit.Parent:FindFirstChild("Humanoid")
\tif humanoid then
\t\thumanoid.Health = 0
\tend
end)
`,
  'Coin Pickup': `-- Collect the parent part to earn a coin (needs the Leaderstats script).
local coin = script.Parent
local Players = game:GetService("Players")
local collected = false

coin.Touched:Connect(function(hit)
\tif collected then return end
\tlocal player = Players:GetPlayerFromCharacter(hit.Parent)
\tif player then
\t\tcollected = true
\t\tlocal stats = player:FindFirstChild("leaderstats")
\t\tif stats and stats:FindFirstChild("Coins") then
\t\t\tstats.Coins.Value = stats.Coins.Value + 1
\t\tend
\t\tcoin:Destroy()
\tend
end)

while coin.Parent do
\tcoin.Orientation = coin.Orientation + Vector3.new(0, 3, 0)
\ttask.wait()
end
`,
  'Leaderstats': `-- Creates a leaderboard entry for each player.
local Players = game:GetService("Players")

Players.PlayerAdded:Connect(function(player)
\tlocal stats = Instance.new("Folder")
\tstats.Name = "leaderstats"
\tstats.Parent = player

\tlocal coins = Instance.new("IntValue")
\tcoins.Name = "Coins"
\tcoins.Parent = stats
end)
`,
  'Spinner': `-- Spins the parent part continuously.
local RunService = game:GetService("RunService")
local part = script.Parent
local speed = 90 -- degrees per second

RunService.Heartbeat:Connect(function(dt)
\tpart.Orientation = part.Orientation + Vector3.new(0, speed * dt, 0)
end)
`,
  'Jump Pad': `-- Launches characters upwards.
local pad = script.Parent
local cooldown = {}

pad.Touched:Connect(function(hit)
\tlocal character = hit.Parent
\tlocal root = character:FindFirstChild("HumanoidRootPart")
\tif root and not cooldown[character] then
\t\tcooldown[character] = true
\t\troot.AssemblyLinearVelocity = Vector3.new(0, 140, 0)
\t\ttask.wait(0.5)
\t\tcooldown[character] = nil
\tend
end)
`,
  'Moving Platform': `-- Moves the parent part back and forth with a tween.
local TweenService = game:GetService("TweenService")
local platform = script.Parent
local start = platform.Position

local info = TweenInfo.new(3, Enum.EasingStyle.Sine, Enum.EasingDirection.InOut, -1, true)
local tween = TweenService:Create(platform, info, { Position = start + Vector3.new(0, 0, 20) })
tween:Play()
`,
  'Day/Night Cycle': `-- Advances the time of day.
local Lighting = game:GetService("Lighting")

while true do
\tLighting.ClockTime = (Lighting.ClockTime + 0.02) % 24
\ttask.wait(0.05)
end
`,
  'Click Door': `-- Opens/closes the parent part when clicked.
local door = script.Parent
local detector = door:FindFirstChildOfClass("ClickDetector")
if not detector then
\tdetector = Instance.new("ClickDetector")
\tdetector.Parent = door
end

local closedPosition = door.Position
local open = false

detector.MouseClick:Connect(function(player)
\topen = not open
\tif open then
\t\tdoor.Position = closedPosition + Vector3.new(0, door.Size.Y, 0)
\t\tdoor.Transparency = 0.5
\telse
\t\tdoor.Position = closedPosition
\t\tdoor.Transparency = 0
\tend
end)
`,
  'Button Counter (LocalScript)': `-- Put this LocalScript inside a TextButton.
local button = script.Parent
local clicks = 0

button.MouseButton1Click:Connect(function()
\tclicks = clicks + 1
\tbutton.Text = "Clicks: " .. clicks
end)
`,
  'Raycast Laser': `-- Turns the part red while something is in front of it (raycast).
local RunService = game:GetService("RunService")
local part = script.Parent

RunService.Heartbeat:Connect(function()
\tlocal result = workspace:Raycast(part.Position, part.CFrame.LookVector * 40)
\tif result then
\t\tpart.Color = Color3.fromRGB(255, 60, 60)
\telse
\t\tpart.Color = Color3.fromRGB(60, 255, 120)
\tend
end)
`,
  'Checkpoint': `-- Touching this part makes it the player's respawn point.
local flag = script.Parent
local Players = game:GetService("Players")

flag.Touched:Connect(function(hit)
\tlocal player = Players:GetPlayerFromCharacter(hit.Parent)
\tif player then
\t\tplayer.RespawnLocation = flag
\t\tflag.Color = Color3.fromRGB(80, 220, 100)
\tend
end)
`,
};

export const ANIMATIONS: Record<string, string> = {
  Spin: JSON.stringify({ duration: 4, easing: 'linear', keyframes: [{ t: 0, rotation: [0, 0, 0] }, { t: 4, rotation: [0, 360, 0] }] }),
  Bob: JSON.stringify({ duration: 2, keyframes: [{ t: 0, position: [0, 0, 0] }, { t: 1, position: [0, 1.5, 0] }, { t: 2, position: [0, 0, 0] }] }),
  Pulse: JSON.stringify({ duration: 1.2, keyframes: [{ t: 0, scale: 1 }, { t: 0.6, scale: 1.25 }, { t: 1.2, scale: 1 }] }),
  Sway: JSON.stringify({ duration: 3, keyframes: [{ t: 0, rotation: [0, 0, -6] }, { t: 1.5, rotation: [0, 0, 6] }, { t: 3, rotation: [0, 0, -6] }] }),
  'Door Swing': JSON.stringify({ duration: 3, keyframes: [{ t: 0, rotation: [0, 0, 0] }, { t: 1, rotation: [0, 90, 0] }, { t: 2, rotation: [0, 90, 0] }, { t: 3, rotation: [0, 0, 0] }] }),
  Hover: JSON.stringify({ duration: 4, keyframes: [{ t: 0, position: [0, 0, 0], rotation: [0, 0, 0] }, { t: 2, position: [0, 2, 0], rotation: [0, 180, 0] }, { t: 4, position: [0, 0, 0], rotation: [0, 360, 0] }] }),
};

const slug = (s: string) => s.toLowerCase().replace(/[^a-z0-9]+/g, '-');
const cache = new Map<string, AssetRecord>();

function make(id: string, name: string, kind: AssetRecord['kind'], mime: string, data: () => string, encoding: 'base64' | 'text'): [string, () => AssetRecord] {
  return [id, () => ({ id, name, kind, mime, data: data(), encoding, created: 0 })];
}

const FACTORIES = new Map<string, () => AssetRecord>([
  ...Object.keys(SYNTH_TEXTURES).map((n) => make(`builtin:texture/${slug(n)}`, n, 'texture', 'image/png', () => renderTexture(n), 'base64')),
  ...Object.keys(SYNTH_SOUNDS).map((n) => make(`builtin:sound/${slug(n)}`, n, 'sound', 'audio/wav', SYNTH_SOUNDS[n], 'base64')),
  ...Object.keys(ANIMATIONS).map((n) => make(`builtin:animation/${slug(n)}`, n, 'animation', 'application/x-forge-animation', () => ANIMATIONS[n], 'text')),
  ...Object.keys(SCRIPT_TEMPLATES).map((n) => make(`builtin:script/${slug(n)}`, n, 'script', 'text/x-lua', () => SCRIPT_TEMPLATES[n], 'text')),
  ...PREFAB_NAMES.map((n) => make(`builtin:model/${slug(n)}`, n, 'model', 'application/x-forge-prefab', () => n, 'text')),
  ...MATERIALS.map((n) => make(`builtin:material/${slug(n)}`, n, 'material', 'application/x-forge-material', () => n, 'text')),
]);

export function getBuiltinAsset(id: string): AssetRecord | undefined {
  if (!id.startsWith('builtin:')) return undefined;
  let a = cache.get(id);
  if (!a) {
    const f = FACTORIES.get(id);
    if (!f) return undefined;
    a = f();
    cache.set(id, a);
  }
  return a;
}

export function listBuiltin(kind?: AssetRecord['kind']): AssetRecord[] {
  const out: AssetRecord[] = [];
  for (const id of FACTORIES.keys()) {
    const a = getBuiltinAsset(id)!;
    if (!kind || a.kind === kind) out.push(a);
  }
  return out;
}

export const builtinId = {
  texture: (n: string) => `builtin:texture/${slug(n)}`,
  sound: (n: string) => `builtin:sound/${slug(n)}`,
  animation: (n: string) => `builtin:animation/${slug(n)}`,
  script: (n: string) => `builtin:script/${slug(n)}`,
  model: (n: string) => `builtin:model/${slug(n)}`,
};

/** Names of the built-in media assets without generating them (for i18n tooling). */
export function builtinNames(): string[] {
  return [...Object.keys(SYNTH_TEXTURES), ...Object.keys(SYNTH_SOUNDS), ...Object.keys(ANIMATIONS)];
}

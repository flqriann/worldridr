// New-project templates and example projects (all generated in code).

import { DataModel, Instance } from '../core/Instance';
import { ensureServices } from '../core/Serializer';
import { udim2, v3, type Vec3 } from '../core/types';
import { buildPrefab } from '../assets/prefabs';
import { SCRIPT_TEMPLATES, builtinId } from '../assets/builtin';
import { generateTerrain } from '../terrain/TerrainTools';
import { Document } from '../editor/Document';
import { msg } from '../i18n';

type Props = Record<string, unknown>;

function add(parent: Instance, className: string, name: string, props: Props = {}): Instance {
  const i = new Instance(className);
  i.props.Name = name;
  Object.assign(i.props, props);
  i.setParent(parent);
  return i;
}

function placePrefab(parent: Instance, name: string, at: Vec3, rename?: string): Instance {
  const m = buildPrefab(name);
  m.walk((i) => {
    if (i.isA('BasePart')) {
      const p = i.props.Position as Vec3;
      i.props.Position = v3(p.x + at.x, p.y + at.y, p.z + at.z);
    }
  });
  if (rename) m.props.Name = rename;
  m.setParent(parent);
  return m;
}

function baseScene(): DataModel {
  const dm = new DataModel();
  ensureServices(dm);
  const ws = dm.workspace;
  add(ws, 'Camera', 'Camera', { Position: v3(0, 14, 30), Orientation: v3(-20, 0, 0) });
  const lighting = dm.getService('Lighting')!;
  add(lighting, 'Sky', 'Sky');
  add(lighting, 'Atmosphere', 'Atmosphere', { Density: 0.25 });
  return dm;
}

function baseplate(ws: Instance) {
  add(ws, 'Part', 'Baseplate', {
    Position: v3(0, -8, 0), Size: v3(512, 16, 512), Anchored: true, Locked: true, Color: '#5d6066',
    TextureId: builtinId.texture('Studs Grid'), TextureTiling: 4,
  });
}

export type TemplateId = 'baseplate' | 'flat-terrain' | 'empty';

export const TEMPLATES: { id: TemplateId; name: string; description: string }[] = [
  { id: 'baseplate', name: msg('Baseplate'), description: msg('A large baseplate with a spawn point.') },
  { id: 'flat-terrain', name: msg('Flat Terrain'), description: msg('Grass terrain ready for sculpting.') },
  { id: 'empty', name: msg('Empty'), description: msg('Only the services – build everything yourself.') },
];

export function createFromTemplate(id: TemplateId): Document {
  const dm = baseScene();
  const ws = dm.workspace;
  if (id === 'baseplate') {
    baseplate(ws);
    add(ws, 'SpawnLocation', 'SpawnLocation', { Position: v3(0, 0.5, 0) });
  } else if (id === 'flat-terrain') {
    const t = dm.terrainInstance!;
    generateTerrain(t.terrain!, { preset: 'flat', seed: 1, amplitude: 0, scale: 100, baseHeight: 0, water: false, waterLevel: 0 });
    add(ws, 'SpawnLocation', 'SpawnLocation', { Position: v3(0, 0.5, 0) });
  }
  const doc = new Document(TEMPLATES.find((t) => t.id === id)!.name, dm);
  doc.history.markSaved();
  return doc;
}

// ---- Example projects --------------------------------------------------------

export const EXAMPLES: { id: string; name: string; description: string }[] = [
  { id: 'obby', name: msg('Obby Course'), description: msg('Obstacle course with kill bricks, moving platforms, a jump pad, coins and a GUI.') },
  { id: 'physics', name: msg('Physics Playground'), description: msg('Unanchored crates, balls and ramps, raycasts and a spawner button.') },
  { id: 'island', name: msg('Island Adventure'), description: msg('Generated terrain island with water, trees, a house, campfire and a day/night cycle.') },
];

export function createExample(id: string): Document {
  const dm = baseScene();
  const ws = dm.workspace;
  const sss = dm.getService('ServerScriptService')!;
  const gui = dm.getService('StarterGui')!;
  let name = 'Example';

  if (id === 'obby') {
    name = 'Obby Course';
    baseplate(ws);
    add(ws, 'SpawnLocation', 'SpawnLocation', { Position: v3(0, 0.5, 0), Color: '#3d8bfd' });
    add(sss, 'Script', 'Leaderstats', { Source: SCRIPT_TEMPLATES['Leaderstats'] });
    const course = add(ws, 'Model', 'Course');
    const colors = ['#ff9f1a', '#ffd43b', '#69db7c', '#4dabf7', '#9775fa', '#f783ac'];
    for (let i = 0; i < 6; i++) {
      add(course, 'Part', `Platform${i + 1}`, { Position: v3(0, 2 + i * 3, -14 - i * 10), Size: v3(6, 1, 6), Anchored: true, Color: colors[i], Material: 'SmoothPlastic' });
    }
    const kb = buildPrefab('Kill Brick');
    kb.walk((i) => { if (i.isA('BasePart')) i.props.Position = v3(0, 0.6, -40); });
    kb.props.Name = 'Lava';
    const lava = kb.children[0];
    lava.props.Size = v3(40, 1, 50);
    kb.setParent(course);
    placePrefab(course, 'Checkpoint Flag', v3(0, 17.5, -64));
    placePrefab(course, 'Moving Platform', v3(0, 12, -96));
    add(course, 'Part', 'Ledge', { Position: v3(0, 20, -112), Size: v3(10, 1, 10), Anchored: true, Color: '#868e96', Material: 'Concrete' });
    placePrefab(course, 'Jump Pad', v3(0, 20.5, -112));
    const finish = add(ws, 'Part', 'Finish', { Position: v3(0, 44, -130), Size: v3(14, 1, 14), Anchored: true, Color: '#ffcc33', Material: 'Neon' });
    add(finish, 'PointLight', 'Glow', { Color: '#ffcc33', Range: 30, Brightness: 2 });
    for (let i = 0; i < 5; i++) placePrefab(ws, 'Coin', v3(0, 2.5 + i * 3, -14 - i * 10), `Coin${i + 1}`);
    placePrefab(ws, 'Coin', v3(0, 44.5, -130), 'GoldCoin');

    const sg = add(gui, 'ScreenGui', 'HUD');
    const label = add(sg, 'TextLabel', 'Message', {
      Position: udim2(0.5, -180, 0, 16), Size: udim2(0, 360, 0, 44), Text: 'Reach the golden platform!',
      BackgroundColor3: '#1d1f24', BackgroundTransparency: 0.25, TextColor3: '#ffffff', TextSize: 22, CornerRadius: 10, Font: 'SansBold',
    });
    add(label, 'LocalScript', 'FinishWatcher', { Source: `-- Updates the message when the player reaches the finish.
local label = script.Parent
local finish = workspace:WaitForChild("Finish")
local Players = game:GetService("Players")
local done = false

finish.Touched:Connect(function(hit)
\tif not done and hit.Parent == Players.LocalPlayer.Character then
\t\tdone = true
\t\tlabel.Text = "You win! Time: " .. string.format("%.1f", time()) .. " s"
\t\tlabel.BackgroundColor3 = Color3.fromRGB(40, 160, 70)
\tend
end)
` });
  } else if (id === 'physics') {
    name = 'Physics Playground';
    baseplate(ws);
    add(ws, 'SpawnLocation', 'SpawnLocation', { Position: v3(0, 0.5, 20) });
    placePrefab(ws, 'Crate Stack', v3(-10, 0, -10));
    placePrefab(ws, 'Crate Stack', v3(10, 0, -10));
    add(ws, 'WedgePart', 'Ramp', { Position: v3(0, 3, -30), Size: v3(12, 6, 16), Anchored: true, Color: '#4dabf7', Material: 'DiamondPlate', Orientation: v3(0, 180, 0) });
    const balls = add(ws, 'Folder', 'Balls');
    for (let i = 0; i < 6; i++) add(balls, 'Part', `Ball${i + 1}`, { Shape: 'Ball', Size: v3(3, 3, 3), Position: v3(-8 + i * 3.2, 12 + i * 2, -26), Color: ['#ff6b6b', '#ffd43b', '#69db7c', '#4dabf7', '#9775fa', '#f783ac'][i], Elasticity: 0.7 });
    const laser = add(ws, 'Part', 'Laser', { Position: v3(-20, 2, 0), Size: v3(1, 1, 2), Anchored: true, Material: 'Neon', Color: '#69db7c', Orientation: v3(0, -90, 0) });
    add(laser, 'Script', 'LaserScript', { Source: SCRIPT_TEMPLATES['Raycast Laser'] });
    add(sss, 'Script', 'BallDropper', { Source: `-- Drops a ball every 3 seconds and removes it after 15 s.
local Debris = game:GetService("Debris")

while true do
\tlocal ball = Instance.new("Part")
\tball.Shape = Enum.PartType.Ball
\tball.Size = Vector3.new(2, 2, 2)
\tball.Color = Color3.fromHSV(math.random(), 0.7, 1)
\tball.Position = Vector3.new(math.random(-6, 6), 30, -30)
\tball.Parent = workspace
\tDebris:AddItem(ball, 15)
\ttask.wait(3)
end
` });
    const sg = add(gui, 'ScreenGui', 'Controls');
    const btn = add(sg, 'TextButton', 'SpawnCrate', { Position: udim2(0, 16, 1, -64), Size: udim2(0, 180, 0, 44), Text: 'Spawn crate', CornerRadius: 8, Font: 'SansBold', TextSize: 20 });
    add(btn, 'LocalScript', 'Spawner', { Source: `-- Spawns a crate in front of the player when the button is clicked.
local Players = game:GetService("Players")
local button = script.Parent

button.MouseButton1Click:Connect(function()
\tlocal character = Players.LocalPlayer.Character
\tif not character then return end
\tlocal root = character:FindFirstChild("HumanoidRootPart")
\tlocal crate = Instance.new("Part")
\tcrate.Size = Vector3.new(3, 3, 3)
\tcrate.Material = Enum.Material.WoodPlanks
\tcrate.Color = Color3.fromRGB(176, 122, 64)
\tcrate.Position = root.Position + root.CFrame.LookVector * 8 + Vector3.new(0, 6, 0)
\tcrate.Parent = workspace
end)
` });
  } else if (id === 'island') {
    name = 'Island Adventure';
    const t = dm.terrainInstance!;
    t.props.WaterLevel = 4;
    generateTerrain(t.terrain!, { preset: 'island', seed: 7, amplitude: 60, scale: 140, baseHeight: -10, water: true, waterLevel: 4 });
    const h = (x: number, z: number) => t.terrain!.heightAt(x, z) ?? 0;
    add(ws, 'SpawnLocation', 'SpawnLocation', { Position: v3(0, h(0, 0) + 0.5, 0), Size: v3(8, 1, 8), Color: '#d9cfbf', Material: 'WoodPlanks' });
    const trees = add(ws, 'Folder', 'Trees');
    let seed = 3;
    const rnd = () => { seed = (seed * 16807) % 2147483647; return seed / 2147483647; };
    for (let k = 0; k < 30; k++) {
      const a = rnd() * Math.PI * 2, r = 30 + rnd() * 120;
      const x = Math.cos(a) * r, z = Math.sin(a) * r, y = h(x, z);
      if (y > 6) placePrefab(trees, rnd() > 0.5 ? 'Tree' : 'Pine Tree', v3(x, y - 0.5, z));
    }
    placePrefab(ws, 'House', v3(30, h(30, 20) - 0.5, 20));
    placePrefab(ws, 'Campfire', v3(10, h(10, -12), -12));
    for (let i = 0; i < 6; i++) { const x = -20 + i * 8, z = -30; placePrefab(ws, 'Coin', v3(x, h(x, z), z), `Coin${i + 1}`); }
    add(sss, 'Script', 'Leaderstats', { Source: SCRIPT_TEMPLATES['Leaderstats'] });
    add(sss, 'Script', 'DayNight', { Source: SCRIPT_TEMPLATES['Day/Night Cycle'] });
    const amb = add(dm.getService('SoundService')!, 'Sound', 'Wind', { SoundId: builtinId.sound('Wind'), Looped: true, Playing: true, Volume: 0.25 });
    void amb;
  }
  const doc = new Document(name, dm);
  doc.history.markSaved();
  return doc;
}

// Built-in prefab models. Each builder returns a detached Model whose lowest
// point sits at y = 0 around the origin; the editor moves it into place.

import { Instance } from '../core/Instance';
import { v3, type Vec3 } from '../core/types';
import { SCRIPT_TEMPLATES, builtinId } from './builtin';

type Props = Record<string, unknown>;

function inst(className: string, name: string, props: Props = {}, parent?: Instance): Instance {
  const i = new Instance(className);
  i.props.Name = name;
  for (const [k, v] of Object.entries(props)) i.props[k] = v;
  if (parent) { i.parent = parent; parent.children.push(i); }
  return i;
}
const part = (parent: Instance, name: string, pos: Vec3, size: Vec3, props: Props = {}) =>
  inst('Part', name, { Position: pos, Size: size, Anchored: true, ...props }, parent);
const script = (parent: Instance, template: string, name = 'Script') =>
  inst('Script', name, { Source: SCRIPT_TEMPLATES[template] }, parent);

const BUILDERS: Record<string, () => Instance> = {
  Tree: () => {
    const m = inst('Model', 'Tree');
    part(m, 'Trunk', v3(0, 4, 0), v3(8, 2, 2), { Shape: 'Cylinder', Orientation: v3(0, 0, 90), Color: '#6b4426', Material: 'Wood' });
    part(m, 'Leaves', v3(0, 10, 0), v3(9, 9, 9), { Shape: 'Ball', Color: '#3f8f3a', Material: 'Grass' });
    part(m, 'Leaves2', v3(2.5, 8.5, 1.5), v3(5, 5, 5), { Shape: 'Ball', Color: '#4fa046', Material: 'Grass' });
    return m;
  },
  'Pine Tree': () => {
    const m = inst('Model', 'Pine Tree');
    part(m, 'Trunk', v3(0, 2.5, 0), v3(5, 1.5, 1.5), { Shape: 'Cylinder', Orientation: v3(0, 0, 90), Color: '#5b3a1f', Material: 'Wood' });
    [[5.5, 9], [8, 7], [10.5, 5], [12.8, 3]].forEach(([y, w], k) =>
      part(m, `Needles${k + 1}`, v3(0, y, 0), v3(2.4, w, w), { Shape: 'Cylinder', Orientation: v3(0, 0, 90), Color: '#2f6b3a', Material: 'Grass' }));
    return m;
  },
  House: () => {
    const m = inst('Model', 'House');
    const wall = { Color: '#d9cfbf', Material: 'Brick' };
    part(m, 'Floor', v3(0, 0.5, 0), v3(20, 1, 16), { Color: '#8a6a4a', Material: 'WoodPlanks' });
    part(m, 'WallBack', v3(0, 6, -7.5), v3(20, 10, 1), wall);
    part(m, 'WallLeft', v3(-9.5, 6, 0), v3(1, 10, 14), wall);
    part(m, 'WallRight', v3(9.5, 6, 0), v3(1, 10, 14), wall);
    part(m, 'WallFrontL', v3(-6.25, 6, 7.5), v3(7.5, 10, 1), wall);
    part(m, 'WallFrontR', v3(6.25, 6, 7.5), v3(7.5, 10, 1), wall);
    part(m, 'WallFrontTop', v3(0, 9.5, 7.5), v3(5, 3, 1), wall);
    const door = part(m, 'Door', v3(0, 4, 7.5), v3(5, 7, 0.6), { Color: '#6b4426', Material: 'Wood' });
    inst('ClickDetector', 'ClickDetector', {}, door);
    script(door, 'Click Door', 'DoorScript');
    inst('WedgePart', 'RoofFront', { Anchored: true, Position: v3(0, 13, 4.25), Size: v3(22, 4, 8.5), Orientation: v3(0, 180, 0), Color: '#9c3b2e', Material: 'Slate' }, m);
    inst('WedgePart', 'RoofBack', { Anchored: true, Position: v3(0, 13, -4.25), Size: v3(22, 4, 8.5), Color: '#9c3b2e', Material: 'Slate' }, m);
    const lamp = part(m, 'CeilingLamp', v3(0, 10.6, 0), v3(1.5, 0.4, 1.5), { Material: 'Neon', Color: '#fff2c4', CastShadow: false });
    inst('PointLight', 'PointLight', { Range: 20, Brightness: 1.5, Color: '#ffe0a8' }, lamp);
    return m;
  },
  'Lamp Post': () => {
    const m = inst('Model', 'Lamp Post');
    part(m, 'Base', v3(0, 0.5, 0), v3(2, 1, 2), { Color: '#2f3338', Material: 'Metal' });
    part(m, 'Pole', v3(0, 6, 0), v3(10, 0.6, 0.6), { Shape: 'Cylinder', Orientation: v3(0, 0, 90), Color: '#2f3338', Material: 'Metal' });
    const bulb = part(m, 'Bulb', v3(0, 11.5, 0), v3(1.6, 1.6, 1.6), { Shape: 'Ball', Material: 'Neon', Color: '#ffe9a8', CastShadow: false });
    inst('PointLight', 'PointLight', { Range: 28, Brightness: 2, Color: '#ffd98a', Shadows: true }, bulb);
    return m;
  },
  Staircase: () => {
    const m = inst('Model', 'Staircase');
    for (let i = 0; i < 10; i++) part(m, `Step${i + 1}`, v3(0, 0.5 + i, -i * 1.5), v3(6, 1, 1.5), { Color: '#8c8f94', Material: 'Concrete' });
    return m;
  },
  Bridge: () => {
    const m = inst('Model', 'Bridge');
    for (let i = 0; i < 12; i++) part(m, `Plank${i + 1}`, v3(0, 4, -11 + i * 2), v3(8, 0.5, 1.8), { Color: '#8a5d36', Material: 'WoodPlanks' });
    part(m, 'RailL', v3(-4, 5.5, 0), v3(0.4, 0.4, 24), { Color: '#5b3a1f', Material: 'Wood' });
    part(m, 'RailR', v3(4, 5.5, 0), v3(0.4, 0.4, 24), { Color: '#5b3a1f', Material: 'Wood' });
    for (const z of [-12, 12]) for (const x of [-4, 4]) part(m, 'Post', v3(x, 3, z), v3(0.8, 6, 0.8), { Color: '#5b3a1f', Material: 'Wood' });
    return m;
  },
  Coin: () => {
    const m = inst('Model', 'Coin');
    const c = part(m, 'Coin', v3(0, 2.5, 0), v3(0.4, 2.5, 2.5), { Shape: 'Cylinder', Color: '#ffcc33', Material: 'Neon', CanCollide: false, Orientation: v3(0, 90, 0) });
    script(c, 'Coin Pickup', 'CoinScript');
    return m;
  },
  'Kill Brick': () => {
    const m = inst('Model', 'Kill Brick');
    const p = part(m, 'KillBrick', v3(0, 0.5, 0), v3(8, 1, 8), { Color: '#ff3b30', Material: 'Neon' });
    script(p, 'Kill Brick', 'KillScript');
    return m;
  },
  'Jump Pad': () => {
    const m = inst('Model', 'Jump Pad');
    const p = part(m, 'JumpPad', v3(0, 0.3, 0), v3(0.6, 6, 6), { Shape: 'Cylinder', Orientation: v3(0, 0, 90), Color: '#2ee6a6', Material: 'Neon' });
    script(p, 'Jump Pad', 'JumpPadScript');
    return m;
  },
  'Moving Platform': () => {
    const m = inst('Model', 'Moving Platform');
    const p = part(m, 'Platform', v3(0, 6, 0), v3(10, 1, 10), { Color: '#3d8bfd', Material: 'DiamondPlate' });
    script(p, 'Moving Platform', 'MoveScript');
    return m;
  },
  Campfire: () => {
    const m = inst('Model', 'Campfire');
    for (let i = 0; i < 4; i++) part(m, `Log${i}`, v3(0, 0.5, 0), v3(4, 0.8, 0.8), { Shape: 'Cylinder', Orientation: v3(0, i * 45, 0), Color: '#5b3a1f', Material: 'Wood' });
    const fire = part(m, 'FireCore', v3(0, 1.2, 0), v3(1, 1, 1), { Transparency: 1, CanCollide: false, CastShadow: false });
    inst('ParticleEmitter', 'Flames', { Rate: 40, Lifetime: 1.2, Speed: 4, SpreadAngle: 20, Acceleration: v3(0, 3, 0), Size: 1.6, SizeEnd: 0.1, Color: '#ffd27a', ColorEnd: '#ff3d00' }, fire);
    inst('ParticleEmitter', 'Smoke', { Rate: 8, Lifetime: 4, Speed: 3, SpreadAngle: 15, Size: 1.5, SizeEnd: 5, Color: '#555555', ColorEnd: '#999999', Transparency: 0.4, TransparencyEnd: 1, Shape: 'Smoke', LightEmission: 0 }, fire);
    inst('PointLight', 'Glow', { Color: '#ff9a3d', Brightness: 3, Range: 24, Shadows: true }, fire);
    inst('Sound', 'Crackle', { SoundId: builtinId.sound('Fire'), Looped: true, Playing: true, Volume: 0.4, RollOffMaxDistance: 60 }, fire);
    return m;
  },
  Spinner: () => {
    const m = inst('Model', 'Spinner');
    part(m, 'Hub', v3(0, 1, 0), v3(2, 2.5, 2.5), { Shape: 'Cylinder', Orientation: v3(0, 0, 90), Color: '#333333', Material: 'Metal' });
    const bar = part(m, 'Bar', v3(0, 1.5, 0), v3(24, 1, 1), { Color: '#ff3b30', Material: 'Neon' });
    script(bar, 'Spinner', 'SpinScript');
    script(bar, 'Kill Brick', 'KillScript');
    return m;
  },
  Door: () => {
    const m = inst('Model', 'Door');
    part(m, 'FrameL', v3(-3, 4, 0), v3(1, 8, 1), { Color: '#5b3a1f', Material: 'Wood' });
    part(m, 'FrameR', v3(3, 4, 0), v3(1, 8, 1), { Color: '#5b3a1f', Material: 'Wood' });
    part(m, 'FrameTop', v3(0, 8.5, 0), v3(7, 1, 1), { Color: '#5b3a1f', Material: 'Wood' });
    const d = part(m, 'Door', v3(0, 4, 0), v3(5, 8, 0.5), { Color: '#a0703f', Material: 'WoodPlanks' });
    inst('ClickDetector', 'ClickDetector', {}, d);
    script(d, 'Click Door', 'DoorScript');
    return m;
  },
  'Checkpoint Flag': () => {
    const m = inst('Model', 'Checkpoint');
    const pad = part(m, 'Pad', v3(0, 0.25, 0), v3(6, 0.5, 6), { Color: '#cccccc', Material: 'SmoothPlastic' });
    script(pad, 'Checkpoint', 'CheckpointScript');
    part(m, 'Pole', v3(2.5, 4, 2.5), v3(0.3, 7, 0.3), { Color: '#eeeeee', Material: 'Metal' });
    part(m, 'Flag', v3(2.5, 6.6, 1.5), v3(0.1, 1.6, 2), { Color: '#ff4040', Material: 'Fabric' });
    return m;
  },
  'Crate Stack': () => {
    const m = inst('Model', 'Crate Stack');
    const crate = (x: number, y: number, z: number, k: number) =>
      part(m, `Crate${k}`, v3(x, y, z), v3(3, 3, 3), { Anchored: false, Color: '#b07a40', Material: 'WoodPlanks' });
    let k = 0;
    for (let y = 0; y < 3; y++) for (let x = 0; x < 3 - y; x++) crate(x * 3.1 - (2 - y) * 1.55, 1.5 + y * 3.05, 0, k++);
    return m;
  },
};

export function buildPrefab(name: string): Instance {
  const b = BUILDERS[name];
  if (!b) throw new Error(`Unknown prefab "${name}"`);
  return b();
}

export const PREFAB_THUMB_COLORS: Record<string, string> = {
  Tree: '#3f8f3a', 'Pine Tree': '#2f6b3a', House: '#9c3b2e', 'Lamp Post': '#ffd98a', Staircase: '#8c8f94', Bridge: '#8a5d36',
  Coin: '#ffcc33', 'Kill Brick': '#ff3b30', 'Jump Pad': '#2ee6a6', 'Moving Platform': '#3d8bfd', Campfire: '#ff9a3d',
  Spinner: '#ff3b30', Door: '#a0703f', 'Checkpoint Flag': '#ff4040', 'Crate Stack': '#b07a40',
};

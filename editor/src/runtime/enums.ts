// Enum definitions exposed to Lua as Enum.<Type>.<Item>, plus the mapping
// from enum-typed properties to their Enum type.

import { MATERIALS } from '../core/classes';
import { TERRAIN_MATERIALS } from '../terrain/TerrainData';

const DIGITS = ['Zero', 'One', 'Two', 'Three', 'Four', 'Five', 'Six', 'Seven', 'Eight', 'Nine'];

/** KeyboardEvent.code -> Enum.KeyCode name */
export function keyCodeName(code: string): string | null {
  if (/^Key[A-Z]$/.test(code)) return code.slice(3);
  if (/^Digit\d$/.test(code)) return DIGITS[+code.slice(5)];
  if (/^F\d{1,2}$/.test(code)) return code;
  const map: Record<string, string> = {
    Space: 'Space', ShiftLeft: 'LeftShift', ShiftRight: 'RightShift', ControlLeft: 'LeftControl', ControlRight: 'RightControl',
    AltLeft: 'LeftAlt', AltRight: 'RightAlt', Enter: 'Return', Escape: 'Escape', Tab: 'Tab', Backspace: 'Backspace',
    ArrowUp: 'Up', ArrowDown: 'Down', ArrowLeft: 'Left', ArrowRight: 'Right', Minus: 'Minus', Equal: 'Equals', Delete: 'Delete',
  };
  return map[code] ?? null;
}

export const KEYCODES = [
  ...'ABCDEFGHIJKLMNOPQRSTUVWXYZ'.split(''), ...DIGITS, ...Array.from({ length: 12 }, (_, i) => `F${i + 1}`),
  'Space', 'LeftShift', 'RightShift', 'LeftControl', 'RightControl', 'LeftAlt', 'RightAlt', 'Return', 'Escape', 'Tab', 'Backspace',
  'Up', 'Down', 'Left', 'Right', 'Minus', 'Equals', 'Delete', 'Unknown',
];

export const ENUMS: Record<string, string[]> = {
  Material: [...new Set([...MATERIALS, ...TERRAIN_MATERIALS.map((m) => m.name)])],
  PartType: ['Block', 'Ball', 'Cylinder'],
  ParticleShape: ['Circle', 'Spark', 'Smoke', 'Star', 'Square'],
  NormalId: ['Front', 'Back', 'Top', 'Bottom', 'Left', 'Right'],
  CameraType: ['Custom', 'Fixed', 'Scriptable'],
  CameraMode: ['Classic', 'LockFirstPerson'],
  Font: ['Sans', 'SansBold', 'Serif', 'Mono', 'Cartoon'],
  TextXAlignment: ['Left', 'Center', 'Right'],
  EasingStyle: ['Linear', 'Sine', 'Quad', 'Cubic', 'Quart', 'Quint', 'Exponential', 'Circular', 'Back', 'Elastic', 'Bounce'],
  EasingDirection: ['In', 'Out', 'InOut'],
  PlaybackState: ['Begin', 'Playing', 'Paused', 'Completed', 'Cancelled'],
  RaycastFilterType: ['Exclude', 'Include', 'Blacklist', 'Whitelist'],
  UserInputType: ['MouseButton1', 'MouseButton2', 'MouseButton3', 'MouseWheel', 'MouseMovement', 'Keyboard', 'Touch', 'None'],
  UserInputState: ['Begin', 'Change', 'End', 'Cancel', 'None'],
  KeyCode: KEYCODES,
};

/** Enum type for an enum-valued property. */
export function enumTypeFor(className: string, prop: string): string {
  if (prop === 'Shape') return className === 'ParticleEmitter' ? 'ParticleShape' : 'PartType';
  if (prop === 'Face') return 'NormalId';
  return prop;
}

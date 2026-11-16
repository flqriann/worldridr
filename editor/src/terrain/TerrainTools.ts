// Terrain brush / generator options. The actual work runs in the Rust core
// (engine/src/terrain.rs) through TerrainData.

import type { BrushToolId, PresetId, Region, TerrainData } from './TerrainData';
import { msg } from '../i18n';

export type BrushTool = BrushToolId;
export type TerrainPreset = PresetId;

export interface BrushOptions {
  tool: BrushTool;
  /** Brush radius in studs. */
  radius: number;
  /** 0..1 */
  strength: number;
  /** Terrain material id (1..7) used by raise / paint. */
  material: number;
  /** Target height for the flatten brush. */
  flattenHeight: number;
}

export const BRUSH_TOOLS: { id: BrushTool; label: string; hint: string }[] = [
  { id: 'raise', label: msg('Raise'), hint: msg('Raise terrain (adds terrain on empty ground)') },
  { id: 'lower', label: msg('Lower'), hint: msg('Lower terrain') },
  { id: 'smooth', label: msg('Smooth'), hint: msg('Average out bumps') },
  { id: 'flatten', label: msg('Flatten'), hint: msg('Flatten to the height where the stroke started') },
  { id: 'paint', label: msg('Paint'), hint: msg('Paint the selected material') },
  { id: 'addWater', label: msg('Water'), hint: msg('Fill with water at Terrain.WaterLevel') },
  { id: 'removeWater', label: msg('Dry'), hint: msg('Remove water') },
  { id: 'erase', label: msg('Erase'), hint: msg('Remove terrain completely') },
];

export function applyBrush(t: TerrainData, x: number, z: number, o: BrushOptions, dt: number): Region | null {
  return t.brush(o.tool, x, z, o.radius, o.strength, o.material, o.flattenHeight, dt);
}

export interface GenerateOptions {
  preset: TerrainPreset;
  seed: number;
  /** Height range in studs. */
  amplitude: number;
  /** Feature size in studs. */
  scale: number;
  baseHeight: number;
  water: boolean;
  waterLevel: number;
}

export function generateTerrain(t: TerrainData, o: GenerateOptions) { t.generate(o); }

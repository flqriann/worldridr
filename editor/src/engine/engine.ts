// Loader and typed access for the Rust core (engine/ → forge_core.wasm).
// All bulk data lives in WASM linear memory; views are re-created whenever
// the memory grows (growth detaches old ArrayBuffers).

import wasmUrl from './forge_core.wasm?url';

export interface ForgeCoreExports {
  memory: WebAssembly.Memory;
  forge_abi_version(): number;
  forge_alloc(len: number): number;
  forge_free(ptr: number, len: number): void;
  terrain_new(size: number, cell: number): number;
  terrain_free(h: number): void;
  terrain_size(h: number): number;
  terrain_heights(h: number): number;
  terrain_materials(h: number): number;
  terrain_water(h: number): number;
  terrain_has_solid(h: number): number;
  terrain_clear(h: number): void;
  terrain_height_at(h: number, x: number, z: number): number;
  terrain_brush(h: number, tool: number, x: number, z: number, radius: number, strength: number, material: number, flattenH: number, dt: number): number;
  terrain_dirty(h: number): number;
  terrain_generate(h: number, preset: number, seed: number, amplitude: number, scale: number, base: number, water: number, waterLevel: number): void;
  terrain_mesh_chunk(h: number, cx: number, cz: number, chunk: number, decoration: number): number;
  terrain_mesh_water(h: number, level: number): number;
  terrain_mesh_positions(h: number): number;
  terrain_mesh_positions_len(h: number): number;
  terrain_mesh_normals(h: number): number;
  terrain_mesh_colors(h: number): number;
  terrain_mesh_indices(h: number): number;
  terrain_raycast(h: number, ox: number, oy: number, oz: number, dx: number, dy: number, dz: number, max: number): number;
  terrain_normal_at(h: number, x: number, z: number, out: number): void;
}

const ABI_VERSION = 2;
let core: ForgeCoreExports | null = null;
let loading: Promise<ForgeCoreExports> | null = null;

/** Loads the native core once. Must complete before terrain is used. */
export function initEngine(): Promise<ForgeCoreExports> {
  return (loading ??= (async () => {
    const res = await fetch(wasmUrl);
    if (!res.ok) throw new Error(`Could not load the Forge core (${res.status})`);
    const { instance } = await WebAssembly.instantiate(await res.arrayBuffer(), {});
    const ex = instance.exports as unknown as ForgeCoreExports;
    if (ex.forge_abi_version() !== ABI_VERSION) throw new Error(`Forge core ABI mismatch: expected ${ABI_VERSION}, got ${ex.forge_abi_version()} – rebuild with "npm run engine"`);
    core = ex;
    return ex;
  })());
}

export function engine(): ForgeCoreExports {
  if (!core) throw new Error('Forge core not initialized (initEngine() was not awaited)');
  return core;
}

export function isEngineReady() { return !!core; }

export const view = {
  f32: (ptr: number, len: number) => new Float32Array(engine().memory.buffer, ptr, len),
  u8: (ptr: number, len: number) => new Uint8Array(engine().memory.buffer, ptr, len),
  u32: (ptr: number, len: number) => new Uint32Array(engine().memory.buffer, ptr, len),
  i32: (ptr: number, len: number) => new Int32Array(engine().memory.buffer, ptr, len),
};

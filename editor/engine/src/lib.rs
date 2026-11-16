//! Forge Studio native core, compiled to `wasm32-unknown-unknown`.
//!
//! The JS side talks to this module through a small C ABI: opaque handles,
//! raw pointers into linear memory for bulk data (heights, mesh buffers) and
//! plain numbers for everything else. No bindgen glue is required.

pub mod terrain;

use terrain::{Preset, Terrain, Tool};

/// ABI version – bumped when exported signatures change.
#[no_mangle]
pub extern "C" fn forge_abi_version() -> u32 { 2 }

// ------------------------------------------------------------------ memory

/// Allocates `len` bytes inside WASM memory (used by JS to pass buffers in).
#[no_mangle]
pub extern "C" fn forge_alloc(len: usize) -> *mut u8 {
    let mut v = Vec::<u8>::with_capacity(len.max(1));
    let p = v.as_mut_ptr();
    core::mem::forget(v);
    p
}

/// Frees a buffer from `forge_alloc`.
///
/// # Safety
/// `ptr`/`len` must come from a previous `forge_alloc` call.
#[no_mangle]
pub unsafe extern "C" fn forge_free(ptr: *mut u8, len: usize) {
    if !ptr.is_null() { drop(Vec::from_raw_parts(ptr, 0, len.max(1))); }
}

// ----------------------------------------------------------------- terrain

fn t<'a>(h: *mut Terrain) -> &'a mut Terrain {
    // SAFETY: handles are only created by `terrain_new` and freed by `terrain_free`;
    // the JS wrapper never uses a handle after freeing it.
    unsafe { &mut *h }
}

#[no_mangle]
pub extern "C" fn terrain_new(size: u32, cell: f32) -> *mut Terrain { Box::into_raw(Box::new(Terrain::new(size as usize, cell))) }

/// # Safety
/// `h` must be a live handle from `terrain_new`.
#[no_mangle]
pub unsafe extern "C" fn terrain_free(h: *mut Terrain) { if !h.is_null() { drop(Box::from_raw(h)); } }

#[no_mangle] pub extern "C" fn terrain_size(h: *mut Terrain) -> u32 { t(h).size as u32 }
#[no_mangle] pub extern "C" fn terrain_heights(h: *mut Terrain) -> *mut f32 { t(h).heights.as_mut_ptr() }
#[no_mangle] pub extern "C" fn terrain_materials(h: *mut Terrain) -> *mut u8 { t(h).materials.as_mut_ptr() }
#[no_mangle] pub extern "C" fn terrain_water(h: *mut Terrain) -> *mut u8 { t(h).water.as_mut_ptr() }
#[no_mangle] pub extern "C" fn terrain_has_solid(h: *mut Terrain) -> u32 { t(h).has_solid() as u32 }
#[no_mangle] pub extern "C" fn terrain_clear(h: *mut Terrain) { t(h).clear() }

/// Height at a world position, NaN over air.
#[no_mangle]
pub extern "C" fn terrain_height_at(h: *mut Terrain, x: f32, z: f32) -> f32 { t(h).height_at(x, z).unwrap_or(f32::NAN) }

/// Returns 1 if the brush changed anything; the region is available via `terrain_dirty`.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn terrain_brush(h: *mut Terrain, tool: u32, x: f32, z: f32, radius: f32, strength: f32, material: u32, flatten_h: f32, dt: f32) -> u32 {
    match Tool::from_u32(tool) {
        Some(tool) => t(h).brush(tool, x, z, radius, strength, material.min(255) as u8, flatten_h, dt) as u32,
        None => 0,
    }
}

/// Pointer to 4 i32: x0, z0, x1, z1 (inclusive vertex range of the last change).
#[no_mangle]
pub extern "C" fn terrain_dirty(h: *mut Terrain) -> *const i32 { &t(h).dirty as *const terrain::Region as *const i32 }

#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn terrain_generate(h: *mut Terrain, preset: u32, seed: u32, amplitude: f32, scale: f32, base: f32, water: u32, water_level: f32) {
    t(h).generate(Preset::from_u32(preset), seed, amplitude, scale, base, water != 0, water_level)
}

/// Meshes one chunk; returns the index count. Buffers: `terrain_mesh_*`.
#[no_mangle]
pub extern "C" fn terrain_mesh_chunk(h: *mut Terrain, cx: u32, cz: u32, chunk: u32, decoration: u32) -> u32 {
    t(h).mesh_chunk(cx as usize, cz as usize, chunk as usize, decoration != 0) as u32
}
/// Water surface mesh; returns the vertex count (positions only).
#[no_mangle]
pub extern "C" fn terrain_mesh_water(h: *mut Terrain, level: f32) -> u32 { t(h).mesh_water(level) as u32 }
#[no_mangle] pub extern "C" fn terrain_mesh_positions(h: *mut Terrain) -> *const f32 { t(h).mesh.positions.as_ptr() }
#[no_mangle] pub extern "C" fn terrain_mesh_positions_len(h: *mut Terrain) -> u32 { t(h).mesh.positions.len() as u32 }
#[no_mangle] pub extern "C" fn terrain_mesh_normals(h: *mut Terrain) -> *const f32 { t(h).mesh.normals.as_ptr() }
#[no_mangle] pub extern "C" fn terrain_mesh_colors(h: *mut Terrain) -> *const f32 { t(h).mesh.colors.as_ptr() }
#[no_mangle] pub extern "C" fn terrain_mesh_indices(h: *mut Terrain) -> *const u32 { t(h).mesh.indices.as_ptr() }

/// Distance to the surface along the ray, or -1 when nothing is hit.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn terrain_raycast(h: *mut Terrain, ox: f32, oy: f32, oz: f32, dx: f32, dy: f32, dz: f32, max_dist: f32) -> f32 {
    t(h).raycast(ox, oy, oz, dx, dy, dz, max_dist).unwrap_or(-1.0)
}

/// Writes the surface normal at (x, z) into `out` (3 floats).
///
/// # Safety
/// `out` must point to 3 writable f32 values.
#[no_mangle]
pub unsafe extern "C" fn terrain_normal_at(h: *mut Terrain, x: f32, z: f32, out: *mut f32) {
    let n = t(h).normal_at_world(x, z);
    core::ptr::copy_nonoverlapping(n.as_ptr(), out, 3);
}

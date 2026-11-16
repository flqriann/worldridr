//! Heightmap terrain: storage, sculpting brushes, procedural generation,
//! chunked mesh generation and ray queries.

pub const AIR: u8 = 0;
pub const MATERIAL_COUNT: usize = 8;

/// sRGB colors of the terrain materials (index = material id).
const MATERIAL_SRGB: [[u8; 3]; MATERIAL_COUNT] = [
    [0, 0, 0],       // Air
    [0x5a, 0x8f, 0x3a], // Grass
    [0x7a, 0x56, 0x36], // Dirt
    [0xd8, 0xc4, 0x8a], // Sand
    [0x7d, 0x7b, 0x78], // Rock
    [0xee, 0xf3, 0xf7], // Snow
    [0x4e, 0x3b, 0x2a], // Mud
    [0xa9, 0xd4, 0xee], // Ice
];

fn srgb_to_linear(c: u8) -> f32 {
    let c = c as f32 / 255.0;
    if c <= 0.04045 { c / 12.92 } else { ((c + 0.055) / 1.055).powf(2.4) }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Tool { Raise, Lower, Smooth, Flatten, Paint, AddWater, RemoveWater, Erase }

impl Tool {
    pub fn from_u32(v: u32) -> Option<Tool> {
        Some(match v {
            0 => Tool::Raise, 1 => Tool::Lower, 2 => Tool::Smooth, 3 => Tool::Flatten,
            4 => Tool::Paint, 5 => Tool::AddWater, 6 => Tool::RemoveWater, 7 => Tool::Erase,
            _ => return None,
        })
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Preset { Flat, Hills, Mountains, Valleys, Island }

impl Preset {
    pub fn from_u32(v: u32) -> Preset {
        match v { 1 => Preset::Hills, 2 => Preset::Mountains, 3 => Preset::Valleys, 4 => Preset::Island, _ => Preset::Flat }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Region { pub x0: i32, pub z0: i32, pub x1: i32, pub z1: i32 }

/// Output buffers of the last meshing call (read by JS through raw pointers).
#[derive(Default)]
pub struct MeshBuffers {
    pub positions: Vec<f32>,
    pub normals: Vec<f32>,
    pub colors: Vec<f32>,
    pub indices: Vec<u32>,
}

pub struct Terrain {
    pub size: usize,
    pub cell: f32,
    pub heights: Vec<f32>,
    pub materials: Vec<u8>,
    pub water: Vec<u8>,
    pub dirty: Region,
    pub mesh: MeshBuffers,
    scratch: Vec<f32>,
    linear_colors: [[f32; 3]; MATERIAL_COUNT],
}

impl Terrain {
    pub fn new(size: usize, cell: f32) -> Terrain {
        let size = size.clamp(2, 4097);
        let n = size * size;
        let mut linear_colors = [[0.0; 3]; MATERIAL_COUNT];
        for (i, c) in MATERIAL_SRGB.iter().enumerate() {
            linear_colors[i] = [srgb_to_linear(c[0]), srgb_to_linear(c[1]), srgb_to_linear(c[2])];
        }
        Terrain {
            size, cell: cell.max(0.01),
            heights: vec![0.0; n], materials: vec![AIR; n], water: vec![0; n],
            dirty: Region::default(), mesh: MeshBuffers::default(), scratch: Vec::new(), linear_colors,
        }
    }

    #[inline] pub fn idx(&self, ix: usize, iz: usize) -> usize { iz * self.size + ix }
    #[inline] pub fn origin(&self) -> f32 { -((self.size - 1) as f32 * self.cell) / 2.0 }
    #[inline] pub fn extent(&self) -> f32 { (self.size - 1) as f32 * self.cell }
    #[inline] fn world_x(&self, ix: usize) -> f32 { self.origin() + ix as f32 * self.cell }
    #[inline] fn grid(&self, w: f32) -> f32 { (w - self.origin()) / self.cell }

    pub fn has_solid(&self) -> bool { self.materials.iter().any(|&m| m != AIR) }

    pub fn clear(&mut self) {
        self.heights.iter_mut().for_each(|h| *h = 0.0);
        self.materials.iter_mut().for_each(|m| *m = AIR);
        self.water.iter_mut().for_each(|w| *w = 0);
    }

    /// Bilinear height at a world position; `None` over empty (air) cells.
    pub fn height_at(&self, x: f32, z: f32) -> Option<f32> {
        let gx = self.grid(x);
        let gz = self.grid(z);
        if gx < 0.0 || gz < 0.0 { return None; }
        let ix = gx.floor() as usize;
        let iz = gz.floor() as usize;
        if ix + 1 >= self.size || iz + 1 >= self.size { return None; }
        let (i00, i10, i01, i11) = (self.idx(ix, iz), self.idx(ix + 1, iz), self.idx(ix, iz + 1), self.idx(ix + 1, iz + 1));
        let m = &self.materials;
        if m[i00] == AIR && m[i10] == AIR && m[i01] == AIR && m[i11] == AIR { return None; }
        let fx = gx - ix as f32;
        let fz = gz - iz as f32;
        let h = &self.heights;
        Some((h[i00] * (1.0 - fx) + h[i10] * fx) * (1.0 - fz) + (h[i01] * (1.0 - fx) + h[i11] * fx) * fz)
    }

    // ---------------------------------------------------------------- brushes

    /// Applies one brush step at world (x, z). Returns true and sets `dirty` if anything changed.
    #[allow(clippy::too_many_arguments)]
    pub fn brush(&mut self, tool: Tool, x: f32, z: f32, radius: f32, strength: f32, material: u8, flatten_h: f32, dt: f32) -> bool {
        let r = radius.max(self.cell * 0.75);
        let cx = self.grid(x);
        let cz = self.grid(z);
        let rg = r / self.cell;
        let last = (self.size - 1) as f32;
        let x0 = (cx - rg).floor().max(0.0);
        let x1 = (cx + rg).ceil().min(last);
        let z0 = (cz - rg).floor().max(0.0);
        let z1 = (cz + rg).ceil().min(last);
        if x0 > x1 || z0 > z1 { return false; }
        let (x0, x1, z0, z1) = (x0 as usize, x1 as usize, z0 as usize, z1 as usize);
        let rate = strength.clamp(0.0, 1.0) * dt.clamp(0.0, 0.1);
        let material = material.clamp(1, (MATERIAL_COUNT - 1) as u8);
        if tool == Tool::Smooth {
            self.scratch.clear();
            self.scratch.extend_from_slice(&self.heights);
        }
        let mut touched = false;
        for iz in z0..=z1 {
            for ix in x0..=x1 {
                let d = ((ix as f32 - cx).powi(2) + (iz as f32 - cz).powi(2)).sqrt() / rg;
                if d > 1.0 { continue; }
                let w = 0.5 * (1.0 + (core::f32::consts::PI * d).cos());
                let i = self.idx(ix, iz);
                match tool {
                    Tool::Raise => {
                        if self.materials[i] == AIR {
                            if w <= 0.15 { continue; }
                            self.materials[i] = material;
                            self.heights[i] = self.heights[i].max(0.0);
                        }
                        self.heights[i] += w * rate * 40.0;
                    }
                    Tool::Lower => {
                        if self.materials[i] == AIR { continue; }
                        self.heights[i] -= w * rate * 40.0;
                    }
                    Tool::Smooth => {
                        if self.materials[i] == AIR { continue; }
                        let (mut sum, mut n) = (0.0f32, 0.0f32);
                        for dz in -1i32..=1 {
                            for dx in -1i32..=1 {
                                let nx = ix as i32 + dx;
                                let nz = iz as i32 + dz;
                                if nx < 0 || nz < 0 || nx as usize >= self.size || nz as usize >= self.size { continue; }
                                let j = self.idx(nx as usize, nz as usize);
                                if self.materials[j] == AIR { continue; }
                                sum += self.scratch[j];
                                n += 1.0;
                            }
                        }
                        if n > 0.0 { self.heights[i] += (sum / n - self.scratch[i]) * (w * rate * 12.0).min(1.0); }
                    }
                    Tool::Flatten => {
                        if self.materials[i] == AIR {
                            if w <= 0.3 { continue; }
                            self.materials[i] = material;
                        }
                        self.heights[i] += (flatten_h - self.heights[i]) * (w * rate * 12.0).min(1.0);
                    }
                    Tool::Paint => {
                        if self.materials[i] == AIR || w < 0.25 { continue; }
                        self.materials[i] = material;
                    }
                    Tool::AddWater => { if w < 0.25 { continue; } self.water[i] = 1; }
                    Tool::RemoveWater => { if w < 0.25 { continue; } self.water[i] = 0; }
                    Tool::Erase => {
                        if w < 0.35 { continue; }
                        self.materials[i] = AIR;
                        self.water[i] = 0;
                        self.heights[i] = 0.0;
                    }
                }
                touched = true;
            }
        }
        if touched {
            self.dirty = Region { x0: x0 as i32, z0: z0 as i32, x1: x1 as i32, z1: z1 as i32 };
        }
        touched
    }

    // ------------------------------------------------------------- generation

    pub fn generate(&mut self, preset: Preset, seed: u32, amplitude: f32, scale: f32, base: f32, water: bool, water_level: f32) {
        let scale = scale.max(1.0);
        let n = self.size;
        let half_extent = self.extent() * 0.45;
        for iz in 0..n {
            for ix in 0..n {
                let x = self.world_x(ix);
                let z = self.world_x(iz);
                let (u, v) = (x / scale, z / scale);
                let h = match preset {
                    Preset::Flat => 0.0,
                    Preset::Hills => fbm(u, v, seed, 4),
                    Preset::Mountains => { let r = 1.0 - (fbm(u, v, seed, 6) * 2.0 - 1.0).abs(); r * r * 1.2 }
                    Preset::Valleys => (fbm(u, v, seed, 5) * 2.0 - 1.0).abs().powf(0.7),
                    Preset::Island => {
                        let d = (x * x + z * z).sqrt() / half_extent;
                        fbm(u, v, seed, 5) * 0.9 + 0.35 - d * d * 0.9
                    }
                };
                let i = self.idx(ix, iz);
                self.heights[i] = base + h * amplitude;
                self.materials[i] = 1;
                self.water[i] = 0;
            }
        }
        self.auto_paint(if water { water_level } else { f32::NEG_INFINITY }, amplitude + base);
        if water {
            for i in 0..n * n { self.water[i] = (self.heights[i] < water_level) as u8; }
        }
        self.dirty = Region { x0: 0, z0: 0, x1: (n - 1) as i32, z1: (n - 1) as i32 };
    }

    /// Materials by height and slope: sand at the shore, rock on cliffs, snow on peaks.
    pub fn auto_paint(&mut self, water_level: f32, top: f32) {
        let n = self.size;
        for iz in 0..n {
            for ix in 0..n {
                let i = self.idx(ix, iz);
                if self.materials[i] == AIR { continue; }
                let h = self.heights[i];
                let hx = self.heights[self.idx((ix + 1).min(n - 1), iz)] - self.heights[self.idx(ix.saturating_sub(1), iz)];
                let hz = self.heights[self.idx(ix, (iz + 1).min(n - 1))] - self.heights[self.idx(ix, iz.saturating_sub(1))];
                let slope = (hx * hx + hz * hz).sqrt() / (2.0 * self.cell);
                self.materials[i] = if h < water_level + 1.5 {
                    if h < water_level - 3.0 { 6 } else { 3 }
                } else if slope > 0.9 { 4 } else if h > top * 0.88 { 5 } else if slope > 0.55 { 2 } else { 1 };
            }
        }
    }

    // ----------------------------------------------------------------- meshing

    fn normal_at(&self, ix: usize, iz: usize) -> [f32; 3] {
        let n = self.size;
        let i = self.idx(ix, iz);
        let h = self.heights[i];
        let sample = |jx: usize, jz: usize| -> f32 {
            let j = jz * n + jx;
            if self.materials[j] == AIR { h } else { self.heights[j] }
        };
        let l = if ix > 0 { sample(ix - 1, iz) } else { h };
        let r = if ix + 1 < n { sample(ix + 1, iz) } else { h };
        let d = if iz > 0 { sample(ix, iz - 1) } else { h };
        let u = if iz + 1 < n { sample(ix, iz + 1) } else { h };
        let (nx, ny, nz) = (l - r, 2.0 * self.cell, d - u);
        let len = (nx * nx + ny * ny + nz * nz).sqrt();
        [nx / len, ny / len, nz / len]
    }

    /// Builds the mesh of one chunk (`chunk` cells per side) into `self.mesh`.
    /// Returns the number of indices (0 = chunk is empty).
    pub fn mesh_chunk(&mut self, cx: usize, cz: usize, chunk: usize, decoration: bool) -> usize {
        let chunk = chunk.max(1);
        let n = self.size;
        let ix0 = cx * chunk;
        let iz0 = cz * chunk;
        let mut mesh = core::mem::take(&mut self.mesh);
        mesh.positions.clear();
        mesh.normals.clear();
        mesh.colors.clear();
        mesh.indices.clear();
        if ix0 >= n - 1 || iz0 >= n - 1 { self.mesh = mesh; return 0; }
        let ix1 = (ix0 + chunk).min(n - 1);
        let iz1 = (iz0 + chunk).min(n - 1);
        let w = ix1 - ix0 + 1;
        for iz in iz0..=iz1 {
            for ix in ix0..=ix1 {
                let i = self.idx(ix, iz);
                mesh.positions.extend_from_slice(&[self.world_x(ix), self.heights[i], self.world_x(iz)]);
                mesh.normals.extend_from_slice(&self.normal_at(ix, iz));
                let c = self.linear_colors[(self.materials[i] as usize).min(MATERIAL_COUNT - 1)];
                let j = if decoration {
                    let hash = ((ix as u32).wrapping_mul(73_856_093) ^ (iz as u32).wrapping_mul(19_349_663)) & 255;
                    0.92 + hash as f32 / 255.0 * 0.16
                } else { 1.0 };
                mesh.colors.extend_from_slice(&[c[0] * j, c[1] * j, c[2] * j]);
            }
        }
        let m = &self.materials;
        for iz in iz0..iz1 {
            for ix in ix0..ix1 {
                let (a, b, c, d) = (self.idx(ix, iz), self.idx(ix + 1, iz), self.idx(ix, iz + 1), self.idx(ix + 1, iz + 1));
                let la = ((iz - iz0) * w + (ix - ix0)) as u32;
                let (lb, lc, ld) = (la + 1, la + w as u32, la + w as u32 + 1);
                if m[a] != AIR && m[c] != AIR && m[b] != AIR { mesh.indices.extend_from_slice(&[la, lc, lb]); }
                if m[b] != AIR && m[c] != AIR && m[d] != AIR { mesh.indices.extend_from_slice(&[lb, lc, ld]); }
            }
        }
        let count = mesh.indices.len();
        self.mesh = mesh;
        count
    }

    /// Water surface quads at `level` for every cell touching a water vertex.
    /// Positions go into `mesh.positions`; returns the vertex count.
    pub fn mesh_water(&mut self, level: f32) -> usize {
        let n = self.size;
        let mut pos = core::mem::take(&mut self.mesh.positions);
        pos.clear();
        for iz in 0..n - 1 {
            for ix in 0..n - 1 {
                let wtr = &self.water;
                if wtr[self.idx(ix, iz)] == 0 && wtr[self.idx(ix + 1, iz)] == 0 && wtr[self.idx(ix, iz + 1)] == 0 && wtr[self.idx(ix + 1, iz + 1)] == 0 { continue; }
                let (x0, x1, z0, z1) = (self.world_x(ix), self.world_x(ix + 1), self.world_x(iz), self.world_x(iz + 1));
                pos.extend_from_slice(&[x0, level, z0, x0, level, z1, x1, level, z0, x1, level, z0, x0, level, z1, x1, level, z1]);
            }
        }
        let count = pos.len() / 3;
        self.mesh.positions = pos;
        count
    }

    // -------------------------------------------------------------- queries

    /// Distance along a (normalized) ray to the terrain surface, or None.
    pub fn raycast(&self, ox: f32, oy: f32, oz: f32, dx: f32, dy: f32, dz: f32, max_dist: f32) -> Option<f32> {
        let len = (dx * dx + dy * dy + dz * dz).sqrt();
        if len < 1e-9 || !self.has_solid_fast() { return None; }
        let (dx, dy, dz) = (dx / len, dy / len, dz / len);
        // clip the ray to the terrain's XZ bounds
        let (lo, hi) = (self.origin(), self.origin() + self.extent());
        let mut t0 = 0.0f32;
        let mut t1 = max_dist;
        for (o, d) in [(ox, dx), (oz, dz)] {
            if d.abs() < 1e-9 {
                if o < lo || o > hi { return None; }
            } else {
                let (mut a, mut b) = ((lo - o) / d, (hi - o) / d);
                if a > b { core::mem::swap(&mut a, &mut b); }
                t0 = t0.max(a);
                t1 = t1.min(b);
            }
        }
        if t0 > t1 { return None; }
        let step = self.cell * 0.35;
        let above = |t: f32| -> Option<bool> {
            let (x, y, z) = (ox + dx * t, oy + dy * t, oz + dz * t);
            self.height_at(x, z).map(|h| y >= h)
        };
        let mut prev_t = t0;
        let mut prev = above(t0);
        let mut t = t0 + step;
        while t <= t1 + step {
            let tc = t.min(t1);
            let cur = above(tc);
            if let (Some(true), Some(false)) = (prev, cur) {
                // refine the crossing by bisection
                let (mut a, mut b) = (prev_t, tc);
                for _ in 0..24 {
                    let m = 0.5 * (a + b);
                    if above(m) == Some(true) { a = m; } else { b = m; }
                }
                return Some(0.5 * (a + b));
            }
            if prev.is_none() && cur == Some(false) {
                return Some(tc); // entered solid terrain through its side (edge cliff)
            }
            prev = cur;
            prev_t = tc;
            if tc >= t1 { break; }
            t += step;
        }
        None
    }

    fn has_solid_fast(&self) -> bool { self.has_solid() }

    pub fn normal_at_world(&self, x: f32, z: f32) -> [f32; 3] {
        let gx = self.grid(x).round().clamp(0.0, (self.size - 1) as f32) as usize;
        let gz = self.grid(z).round().clamp(0.0, (self.size - 1) as f32) as usize;
        self.normal_at(gx, gz)
    }
}

// ------------------------------------------------------------------ noise

fn hash(ix: i32, iz: i32, seed: u32) -> f32 {
    let mut h = ix.wrapping_mul(374_761_393).wrapping_add(iz.wrapping_mul(668_265_263)).wrapping_add((seed as i32).wrapping_mul(2_147_483_647));
    h = (h ^ ((h as u32) >> 13) as i32).wrapping_mul(1_274_126_177);
    ((h ^ ((h as u32) >> 16) as i32) as u32) as f32 / 4_294_967_295.0
}

fn smooth(t: f32) -> f32 { t * t * (3.0 - 2.0 * t) }

pub fn value_noise(x: f32, z: f32, seed: u32) -> f32 {
    let (ix, iz) = (x.floor() as i32, z.floor() as i32);
    let (fx, fz) = (smooth(x - ix as f32), smooth(z - iz as f32));
    let (a, b, c, d) = (hash(ix, iz, seed), hash(ix + 1, iz, seed), hash(ix, iz + 1, seed), hash(ix + 1, iz + 1, seed));
    (a * (1.0 - fx) + b * fx) * (1.0 - fz) + (c * (1.0 - fx) + d * fx) * fz
}

pub fn fbm(x: f32, z: f32, seed: u32, octaves: u32) -> f32 {
    let (mut amp, mut freq, mut sum, mut norm) = (1.0f32, 1.0f32, 0.0f32, 0.0f32);
    for o in 0..octaves {
        sum += value_noise(x * freq, z * freq, seed.wrapping_add(o * 17)) * amp;
        norm += amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    sum / norm
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn raise_and_height() {
        let mut t = Terrain::new(65, 4.0);
        assert!(!t.has_solid());
        for _ in 0..10 { assert!(t.brush(Tool::Raise, 0.0, 0.0, 12.0, 1.0, 1, 0.0, 0.05)); }
        let h = t.height_at(0.0, 0.0).unwrap();
        assert!(h > 15.0, "height {h}");
        assert!(t.height_at(100.0, 100.0).is_none());
    }

    #[test]
    fn raycast_hits_surface() {
        let mut t = Terrain::new(65, 4.0);
        t.generate(Preset::Flat, 1, 0.0, 100.0, 5.0, false, 0.0);
        let d = t.raycast(3.0, 50.0, -7.0, 0.0, -1.0, 0.0, 200.0).unwrap();
        assert!((d - 45.0).abs() < 0.01, "distance {d}");
        let d2 = t.raycast(-100.0, 20.0, 0.0, 1.0, -0.25, 0.0, 500.0).unwrap();
        assert!((20.0 - 0.25 * d2 / (1.0f32 + 0.0625).sqrt() - 5.0).abs() < 0.05);
        assert!(t.raycast(0.0, 50.0, 0.0, 0.0, 1.0, 0.0, 100.0).is_none());
    }

    #[test]
    fn mesh_chunk_counts() {
        let mut t = Terrain::new(65, 4.0);
        t.generate(Preset::Hills, 3, 20.0, 60.0, 0.0, false, 0.0);
        let idx = t.mesh_chunk(0, 0, 32, true);
        assert_eq!(idx, 32 * 32 * 6);
        assert_eq!(t.mesh.positions.len(), 33 * 33 * 3);
        assert_eq!(t.mesh_chunk(5, 5, 32, true), 0);
    }

    #[test]
    fn smooth_reduces_spike() {
        let mut t = Terrain::new(33, 4.0);
        t.generate(Preset::Flat, 1, 0.0, 100.0, 0.0, false, 0.0);
        let c = t.idx(16, 16);
        t.heights[c] = 40.0;
        for _ in 0..20 { t.brush(Tool::Smooth, 0.0, 0.0, 12.0, 1.0, 1, 0.0, 0.1); }
        assert!(t.heights[c] < 20.0);
    }
}

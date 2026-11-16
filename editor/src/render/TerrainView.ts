// Renders TerrainData as chunked meshes (32x32 cells each, built by the Rust
// core) plus a water surface. Brush strokes only re-mesh the touched chunks.

import * as THREE from 'three';
import type { Instance } from '../core/Instance';
import { CHUNK_CELLS, type Region } from '../terrain/TerrainData';
import { applyTriplanar, terrainDetailTexture } from './materials';

export class TerrainView {
  readonly group = new THREE.Group();
  readonly water: THREE.Mesh;
  private chunks = new Map<string, THREE.Mesh>();
  private waterGeo = new THREE.BufferGeometry();
  private material: THREE.MeshStandardMaterial;
  private waterMat: THREE.MeshStandardMaterial;

  constructor(private inst: Instance) {
    this.material = new THREE.MeshStandardMaterial({ vertexColors: true, roughness: 0.95, metalness: 0 });
    this.material.map = terrainDetailTexture();
    applyTriplanar(this.material, 10, true);
    this.waterMat = new THREE.MeshStandardMaterial({ transparent: true, roughness: 0.08, metalness: 0.2, depthWrite: false });
    this.water = new THREE.Mesh(this.waterGeo, this.waterMat);
    this.water.renderOrder = 2;
    this.water.userData.inst = inst;
    this.group.add(this.water);
    this.rebuild();
  }

  /** Rebuilds everything (or only the chunks overlapping `region`). */
  rebuild(region?: Region | null, waterToo = true) {
    const t = this.inst.terrain!;
    const per = t.chunksPerSide;
    const decoration = this.inst.props.Decoration !== false;
    let cx0 = 0, cz0 = 0, cx1 = per - 1, cz1 = per - 1;
    if (region) {
      // vertex region -> chunk range (neighbors too: normals on the border use adjacent heights)
      cx0 = Math.max(0, Math.floor((region.x0 - 1) / CHUNK_CELLS));
      cz0 = Math.max(0, Math.floor((region.z0 - 1) / CHUNK_CELLS));
      cx1 = Math.min(per - 1, Math.floor((region.x1 + 1) / CHUNK_CELLS));
      cz1 = Math.min(per - 1, Math.floor((region.z1 + 1) / CHUNK_CELLS));
    } else {
      for (const m of this.chunks.values()) { m.removeFromParent(); m.geometry.dispose(); }
      this.chunks.clear();
    }
    for (let cz = cz0; cz <= cz1; cz++) {
      for (let cx = cx0; cx <= cx1; cx++) {
        const key = `${cx},${cz}`;
        const data = t.meshChunk(cx, cz, decoration);
        let mesh = this.chunks.get(key);
        if (!data) {
          if (mesh) { mesh.removeFromParent(); mesh.geometry.dispose(); this.chunks.delete(key); }
          continue;
        }
        const geo = mesh?.geometry ?? new THREE.BufferGeometry();
        geo.setAttribute('position', new THREE.BufferAttribute(data.positions, 3));
        geo.setAttribute('normal', new THREE.BufferAttribute(data.normals, 3));
        geo.setAttribute('color', new THREE.BufferAttribute(data.colors, 3));
        geo.setIndex(new THREE.BufferAttribute(data.indices, 1));
        geo.computeBoundingSphere();
        geo.computeBoundingBox();
        if (!mesh) {
          mesh = new THREE.Mesh(geo, this.material);
          mesh.receiveShadow = true;
          mesh.castShadow = true;
          mesh.userData.inst = this.inst;
          mesh.userData.terrain = true;
          this.chunks.set(key, mesh);
          this.group.add(mesh);
        }
      }
    }
    if (waterToo) this.rebuildWater();
  }

  rebuildWater() {
    const t = this.inst.terrain!;
    const pos = t.waterMesh(this.inst.props.WaterLevel as number);
    this.waterGeo.setAttribute('position', new THREE.BufferAttribute(pos, 3));
    const nrm = new Float32Array(pos.length);
    for (let i = 1; i < nrm.length; i += 3) nrm[i] = 1;
    this.waterGeo.setAttribute('normal', new THREE.BufferAttribute(nrm, 3));
    this.waterGeo.computeBoundingSphere();
    this.water.visible = pos.length > 0;
    this.waterMat.color.set(this.inst.props.WaterColor as string);
    this.waterMat.opacity = 1 - (this.inst.props.WaterTransparency as number);
  }

  get chunkCount() { return this.chunks.size; }

  onPropChanged(prop: string) {
    if (prop === 'WaterLevel') this.rebuildWater();
    else if (prop === 'WaterColor' || prop === 'WaterTransparency') {
      this.waterMat.color.set(this.inst.props.WaterColor as string);
      this.waterMat.opacity = 1 - (this.inst.props.WaterTransparency as number);
    } else if (prop === 'Decoration') this.rebuild(null, false);
  }

  dispose() {
    for (const m of this.chunks.values()) m.geometry.dispose();
    this.chunks.clear();
    this.waterGeo.dispose();
    this.material.dispose();
    this.waterMat.dispose();
  }
}

// glTF/GLB model asset loading with caching. Models are normalized to a unit
// cube so MeshPart.Size directly controls their dimensions.

import * as THREE from 'three';
import { GLTFLoader } from 'three/examples/jsm/loaders/GLTFLoader.js';
import type { AssetRecord } from '../core/Serializer';
import { assetBytes } from '../assets/AssetStore';

export interface LoadedModel { scene: THREE.Object3D; size: THREE.Vector3 }

const cache = new Map<string, Promise<LoadedModel>>();

export function loadModelAsset(a: AssetRecord): Promise<LoadedModel> {
  const key = a.id + ':' + a.data.length;
  let p = cache.get(key);
  if (!p) {
    p = new Promise<LoadedModel>((resolve, reject) => {
      if (a.mime !== 'model/gltf-binary' && a.mime !== 'model/gltf+json') { reject(new Error(`Asset "${a.name}" is not a glTF model`)); return; }
      const loader = new GLTFLoader();
      const data = a.mime === 'model/gltf+json' ? a.data : assetBytes(a);
      loader.parse(data, '', (gltf) => {
        const root = gltf.scene;
        const box = new THREE.Box3().setFromObject(root);
        const size = box.getSize(new THREE.Vector3());
        const center = box.getCenter(new THREE.Vector3());
        const norm = new THREE.Group();
        root.position.sub(center);
        norm.add(root);
        const s = new THREE.Vector3(1 / Math.max(size.x, 1e-4), 1 / Math.max(size.y, 1e-4), 1 / Math.max(size.z, 1e-4));
        norm.scale.copy(s);
        norm.traverse((o) => { if ((o as THREE.Mesh).isMesh) { o.castShadow = true; o.receiveShadow = true; } });
        resolve({ scene: norm, size });
      }, (err) => reject(new Error(`Could not read model "${a.name}": ${(err as unknown as Error)?.message ?? err}`)));
    });
    cache.set(key, p);
    p.catch(() => cache.delete(key));
  }
  return p;
}

/** Natural size (in studs, 1 unit = 1 stud) of a model asset. */
export async function modelNaturalSize(a: AssetRecord): Promise<THREE.Vector3> {
  return (await loadModelAsset(a)).size.clone();
}

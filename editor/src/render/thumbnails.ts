// Offscreen thumbnail renderer for model assets (glTF and built-in prefabs).

import * as THREE from 'three';
import type { AssetRecord } from '../core/Serializer';
import { buildPrefab } from '../assets/prefabs';
import { parseFile, deserializeInstance, LoadReport } from '../core/Serializer';
import type { Instance } from '../core/Instance';
import { loadModelAsset } from './models';
import { GEOMETRY, effectiveSize, orientationToQuat, shapeOf } from './geometry';
import type { Vec3 } from '../core/types';

let renderer: THREE.WebGLRenderer | null = null;
const cache = new Map<string, Promise<string>>();
const SIZE = 128;

function getRenderer() {
  if (!renderer) {
    renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true, preserveDrawingBuffer: true });
    renderer.setSize(SIZE, SIZE);
    renderer.outputColorSpace = THREE.SRGBColorSpace;
  }
  return renderer;
}

function render(obj: THREE.Object3D): string {
  const scene = new THREE.Scene();
  scene.add(new THREE.HemisphereLight(0xffffff, 0x444455, 1.6));
  const sun = new THREE.DirectionalLight(0xffffff, 2);
  sun.position.set(3, 5, 4);
  scene.add(sun, obj);
  const box = new THREE.Box3().setFromObject(obj);
  const c = box.getCenter(new THREE.Vector3());
  const r = Math.max(0.5, box.getSize(new THREE.Vector3()).length() / 2);
  const cam = new THREE.PerspectiveCamera(35, 1, 0.01, r * 20);
  cam.position.copy(c).add(new THREE.Vector3(1, 0.8, 1.2).normalize().multiplyScalar(r * 2.9));
  cam.lookAt(c);
  const rr = getRenderer();
  rr.setClearColor(0x000000, 0);
  rr.render(scene, cam);
  return rr.domElement.toDataURL('image/png');
}

function instancesToObject(roots: Instance[]): THREE.Group {
  const g = new THREE.Group();
  for (const root of roots) root.walk((i) => {
    if (!i.isA('BasePart') || (i.props.Transparency as number) >= 1) return;
    const s = effectiveSize(i);
    const m = new THREE.Mesh(i.className === 'MeshPart' ? GEOMETRY.Block : GEOMETRY[shapeOf(i)], new THREE.MeshStandardMaterial({ color: i.props.Color as string, roughness: 0.6 }));
    m.scale.set(s.x, s.y, s.z);
    m.position.copy(i.props.Position as THREE.Vector3Like);
    m.quaternion.copy(orientationToQuat(i.props.Orientation as Vec3));
    g.add(m);
  });
  return g;
}

export function modelThumbnail(a: AssetRecord): Promise<string> {
  const key = a.id + ':' + a.data.length;
  let p = cache.get(key);
  if (!p) {
    p = (async () => {
      if (a.mime === 'application/x-forge-prefab') return render(instancesToObject([buildPrefab(a.data)]));
      if (a.mime === 'application/x-forge-model') {
        const f = parseFile(a.data);
        if (f.format !== 'forge-model') throw new Error('bad model');
        const rep = new LoadReport();
        return render(instancesToObject(f.instances.map((j) => deserializeInstance(j, rep, false)!).filter(Boolean)));
      }
      const m = await loadModelAsset(a);
      const o = m.scene.clone(true);
      o.scale.multiply(m.size);
      return render(o);
    })();
    cache.set(key, p);
  }
  return p;
}

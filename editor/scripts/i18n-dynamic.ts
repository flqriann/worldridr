// Collects UI strings that come from data tables (class registry, prefabs,
// materials, built-in assets) and are translated with t(variable), so
// scripts/i18n-check.mjs can include them. Bundled with esbuild at check time.
import { PREFAB_NAMES, builtinNames } from '../src/assets/builtin';
import { allClasses, MATERIALS } from '../src/core/classes';
import { TERRAIN_MATERIALS } from '../src/terrain/TerrainData';

const INSERT_GROUPS = ['Parts', 'Containers', 'Scripts', 'Lights', 'Effects', 'Interaction', 'Environment', 'GUI', 'Values'];
const keys = new Set<string>([...PREFAB_NAMES, ...MATERIALS, ...INSERT_GROUPS, 'Block', 'Ball', 'Cylinder', 'Wedge']);
for (const m of TERRAIN_MATERIALS) keys.add(m.name);
for (const n of builtinNames()) keys.add(n);
for (const c of allClasses()) {
  if (c.description) keys.add(c.description);
  for (const p of c.props) keys.add(p.category);
}
console.log(JSON.stringify([...keys]));

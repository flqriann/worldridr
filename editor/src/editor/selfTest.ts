// Built-in self-test (Test ▸ Run Self-Test): exercises the core systems
// headlessly and reports real pass/fail results to the Output.

import { Instance } from '../core/Instance';
import { LoadReport, parseFile } from '../core/Serializer';
import { v3, type Vec3 } from '../core/types';
import { Document } from './Document';
import { createExample, createFromTemplate } from '../project/templates';
import { LuaRuntime, preprocessLuau } from '../runtime/LuaRuntime';
import { Physics } from '../runtime/Physics';
import { applyBrush } from '../terrain/TerrainTools';
import { TerrainData } from '../terrain/TerrainData';
import { formatLua } from '../ui/monacoSetup';

export interface TestResult { name: string; ok: boolean; detail: string }

function assert(cond: unknown, msg: string): asserts cond { if (!cond) throw new Error(msg); }

export async function runSelfTest(log: (r: TestResult) => void): Promise<TestResult[]> {
  const results: TestResult[] = [];
  const test = async (name: string, fn: () => Promise<string | void> | string | void) => {
    let r: TestResult;
    try { r = { name, ok: true, detail: (await fn()) || 'ok' }; } catch (e) { r = { name, ok: false, detail: (e as Error).message }; }
    results.push(r);
    log(r);
  };

  await test('Object system + undo/redo', () => {
    const doc = createFromTemplate('baseplate');
    const ws = doc.dm.workspace;
    const before = ws.children.length;
    const p = new Instance('Part');
    doc.transact('add', () => p.setParent(ws));
    doc.transact('move', () => p.set('Position', v3(1, 2, 3)));
    assert(ws.children.length === before + 1, 'part not added');
    doc.history.undo();
    assert((p.props.Position as Vec3).x !== 1, 'undo did not revert Position');
    doc.history.undo();
    assert(!p.parent && ws.children.length === before, 'undo did not remove part');
    doc.history.redo(); doc.history.redo();
    assert(p.parent === ws && (p.props.Position as Vec3).y === 2, 'redo failed');
    return `${doc.dm.index.size} objects`;
  });

  await test('Project save/load round trip', () => {
    const doc = createExample('obby');
    const text = JSON.stringify(doc.toProjectFile());
    const parsed = parseFile(text);
    assert(parsed.format === 'forge-project', 'wrong format');
    const rep = new LoadReport();
    const copy = Document.fromProjectFile(parsed, rep);
    assert(copy.dm.index.size === doc.dm.index.size, `object count ${copy.dm.index.size} != ${doc.dm.index.size}`);
    const a = doc.dm.workspace.findFirstChild('Finish')!, b = copy.dm.workspace.findFirstChild('Finish')!;
    assert(JSON.stringify(a.props) === JSON.stringify(b.props), 'properties differ after reload');
    assert(rep.warnings.length === 0, 'warnings: ' + rep.warnings.join('; '));
    return `${(text.length / 1024).toFixed(1)} KB, ${copy.dm.index.size} objects`;
  });

  await test('Rejects corrupt / newer files', () => {
    let msg = '';
    try { parseFile('{"format":"forge-project","version":999}'); } catch (e) { msg = (e as Error).message; }
    assert(/newer/.test(msg), 'newer version not rejected');
    try { parseFile('not json'); msg = ''; } catch (e) { msg = (e as Error).message; }
    assert(msg, 'invalid JSON not rejected');
  });

  await test('Lua runtime: API, datatypes, events, scheduler', async () => {
    const doc = createFromTemplate('baseplate');
    const logs: string[] = [];
    const rt = await LuaRuntime.create(doc.dm, { log: (l, m) => logs.push(`${l}:${m}`) });
    try {
      rt.exec(`
        local p = Instance.new("Part")
        p.Name = "LuaPart"
        p.Size = Vector3.new(2, 3, 4)
        p.Color = Color3.fromRGB(255, 0, 0)
        p.Material = Enum.Material.Neon
        p.CFrame = CFrame.new(10, 5, 0) * CFrame.Angles(0, math.rad(90), 0)
        p.Parent = workspace
        local v = (Vector3.new(1, 2, 3) + Vector3.new(1, 1, 1)) * 2
        print(v, typeof(p), p.Material == Enum.Material.Neon, workspace:FindFirstChild("LuaPart") == p)
        p.Changed:Connect(function(prop) if prop == "Transparency" then print("changed", prop) end end)
        task.delay(0.1, function() print("delayed") end)
        local x = 1
        x += 4
        print("x", x)
      `);
      const part = doc.dm.workspace.findFirstChild('LuaPart');
      assert(part, 'Instance.new + Parent failed');
      assert((part.props.Size as Vec3).z === 4 && part.props.Color === '#ff0000' && part.props.Material === 'Neon', 'property assignment failed');
      assert(Math.abs((part.props.Orientation as Vec3).y - 90) < 0.01 && (part.props.Position as Vec3).x === 10, 'CFrame assignment failed');
      part.set('Transparency', 0.5);
      rt.step(0.05, 0.05);
      rt.step(0.1, 0.2);
      const out = logs.join('\n');
      assert(out.includes('print:4, 6, 8 Instance true true'), 'datatype/print mismatch: ' + out);
      assert(out.includes('print:changed Transparency'), 'Changed event not delivered');
      assert(out.includes('print:delayed'), 'task.delay did not run');
      assert(out.includes('print:x 5'), 'compound assignment failed');
      rt.exec('local a = nil; print(a.b)');
      assert(logs.some((l) => l.startsWith('error:') && l.includes('attempt to index')), 'runtime error not reported');
      rt.exec('error("custom failure")');
      assert(logs.some((l) => l.includes('custom failure')), 'error() not reported');
      const syn = await LuaRuntime.checkSyntax('local x = = 1');
      assert(syn && syn.line === 1, 'syntax error not detected');
      return `${logs.length} log lines`;
    } finally { rt.dispose(); }
  });

  await test('Lua runtime: infinite loop protection', async () => {
    const doc = createFromTemplate('empty');
    const logs: string[] = [];
    const rt = await LuaRuntime.create(doc.dm, { log: (l, m) => logs.push(`${l}:${m}`) });
    try {
      const t0 = performance.now();
      rt.exec('while true do end');
      const ms = performance.now() - t0;
      assert(logs.some((l) => l.includes('timeout')), 'endless loop was not stopped');
      return `stopped after ${(ms / 1000).toFixed(1)} s`;
    } finally { rt.dispose(); }
  });

  await test('Physics: gravity, anchoring, raycast', () => {
    const doc = createFromTemplate('baseplate');
    const ws = doc.dm.workspace;
    const ball = new Instance('Part');
    Object.assign(ball.props, { Shape: 'Ball', Size: v3(2, 2, 2), Position: v3(40, 20, 0), Anchored: false });
    ball.setParent(ws);
    const phys = new Physics(doc.dm);
    phys.build();
    for (let i = 0; i < 180; i++) { phys.step(1 / 60); phys.syncToInstances(); }
    const y = (ball.props.Position as Vec3).y;
    assert(y < 1.2 && y > 0.8, `ball (radius 1) should rest on the baseplate top at y=0 (y=${y.toFixed(2)})`);
    const hit = phys.raycast(v3(40, 50, 0), v3(0, -100, 0), new Set(), 'Exclude');
    assert(hit && hit.inst === ball, 'raycast should hit the ball first');
    phys.dispose();
    return `ball rests at y=${y.toFixed(2)}`;
  });

  await test('Terrain brush + undo snapshot', () => {
    const doc = createFromTemplate('flat-terrain');
    const t = doc.dm.terrainInstance!;
    const before = t.terrain!.snapshot();
    for (let i = 0; i < 10; i++) applyBrush(t.terrain!, 0, 0, { tool: 'raise', radius: 10, strength: 1, material: 4, flattenHeight: 0 }, 0.05);
    const h = t.terrain!.heightAt(0, 0)!;
    assert(h > 5, `raise brush had no effect (h=${h})`);
    doc.transact('Terrain', () => doc.history.record({ k: 'terrain', inst: t, before, after: t.terrain!.snapshot() }));
    doc.history.undo();
    assert((t.terrain!.heightAt(0, 0) ?? 0) === 0, 'terrain undo failed');
    return `raised to ${h.toFixed(1)} studs`;
  });

  await test('Rust core: generation, meshing, raycast (WASM)', () => {
    const t = new TerrainData(257, 4);
    const t0 = performance.now();
    t.generate({ preset: 'hills', seed: 5, amplitude: 40, scale: 120, baseHeight: 0, water: true, waterLevel: 8 });
    let tris = 0;
    for (let cz = 0; cz < t.chunksPerSide; cz++) for (let cx = 0; cx < t.chunksPerSide; cx++) tris += (t.meshChunk(cx, cz, true)?.indices.length ?? 0) / 3;
    const ms = performance.now() - t0;
    assert(tris === 256 * 256 * 2, `unexpected triangle count ${tris}`);
    const h = t.heightAt(10, 10)!;
    const d = t.raycast(10, 200, 10, 0, -1, 0, 1000);
    assert(d !== null && Math.abs(200 - d - h) < 0.05, `raycast ${d} vs height ${h}`);
    const h00 = t.heightAt(0, 0)!;
    const before = t.snapshot({ x0: 100, z0: 100, x1: 140, z1: 140 });
    const r = t.brush('raise', 0, 0, 20, 1, 4, 0, 0.05);
    assert(r && r.x0 >= 100 && r.x1 <= 140, 'brush region outside the expected rectangle');
    assert(t.heightAt(0, 0)! > h00, 'raise had no effect');
    t.restore(before);
    assert(Math.abs(t.heightAt(0, 0)! - h00) < 1e-4, 'regional restore failed');
    return `${tris.toLocaleString()} triangles generated + meshed in ${ms.toFixed(0)} ms`;
  });

  await test('Lua formatter + Luau preprocessing', () => {
    const f = formatLua('if a then\nprint(1)\nelse\nprint(2)\nend\npart.Touched:Connect(function(hit)\nprint(hit)\nend)');
    assert(f === 'if a then\n\tprint(1)\nelse\n\tprint(2)\nend\npart.Touched:Connect(function(hit)\n\tprint(hit)\nend)', 'unexpected formatting:\n' + f);
    assert(preprocessLuau('  x.y += 2 -- c') === '  x.y = x.y + (2) -- c', 'compound assignment rewrite failed');
  });

  return results;
}

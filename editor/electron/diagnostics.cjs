// Smoke test (--smoke-test) and benchmark (--bench) for the Studio window.
const { app } = require('electron');

function runSmokeTest(win) {
  win.webContents.on('console-message', (_e, level, message) => { if (level >= 2) console.log('[renderer]', message); });
  win.webContents.once('did-finish-load', async () => {
    try {
      const result = await win.webContents.executeJavaScript(`(async () => {
        const wait = (ms) => new Promise((r) => setTimeout(r, ms));
        for (let i = 0; i < 100 && !window.forgeStudio; i++) await wait(100);
        const s = window.forgeStudio;
        const tests = await s.runSelfTest();
        s.openExample('obby');
        await wait(500);
        await s.startPlay();
        for (let i = 0; i < 120; i++) { s.viewport.beforeRender.fire(1 / 60); }
        const ch = s.play && s.play.character;
        const play = { started: !!s.play, errors: s.play ? s.play.errors : -1, y: ch ? ch.root.props.Position.y : null };
        await s.stopPlay();
        return { tests: tests.map((t) => (t.ok ? 'PASS ' : 'FAIL ') + t.name + ' - ' + t.detail), play, electron: !!window.forgeNative };
      })()`);
      console.log(JSON.stringify(result, null, 2));
      const ok = result.tests.every((t) => t.startsWith('PASS')) && result.play.started && result.play.errors === 0;
      console.log(ok ? 'SMOKE TEST PASSED' : 'SMOKE TEST FAILED');
      app.exit(ok ? 0 : 1);
    } catch (e) {
      console.error('SMOKE TEST ERROR', e);
      app.exit(2);
    }
  });
}

// Performance benchmark: 3000 parts (1000 unanchored) in edit and play mode.
function runBenchmark(win) {
  win.webContents.once('did-finish-load', async () => {
    try {
      const result = await win.webContents.executeJavaScript(`(async () => {
        const wait = (ms) => new Promise((r) => setTimeout(r, ms));
        for (let i = 0; i < 100 && !window.forgeStudio; i++) await wait(100);
        const s = window.forgeStudio;
        document.querySelector('.modal-backdrop .btn.danger')?.click();
        s.newProject('baseplate');
        await wait(300);
        await s.runCommand('for i = 1, 3000 do local p = Instance.new("Part"); p.Size = Vector3.new(2,2,2); p.Anchored = (i % 3 ~= 0); p.Position = Vector3.new((i % 50) * 3 - 75, 2 + math.floor(i / 50) * 2.2, (i % 7) * 3); p.Color = Color3.fromHSV((i % 20) / 20, 0.6, 1); p.Parent = workspace end');
        const vp = s.viewport;
        vp.camPos.set(0, 60, 140); vp.pitch = -0.35; vp.yaw = 0;
        const frames = async (n) => { const t = []; let last = performance.now(); for (let i = 0; i < n; i++) { await new Promise((r) => requestAnimationFrame(r)); const now = performance.now(); t.push(now - last); last = now; } t.sort((a, b) => a - b); return { avg: +(t.reduce((a, b) => a + b, 0) / n).toFixed(1), p95: +t[Math.floor(n * 0.95)].toFixed(1) }; };
        await frames(30);
        const edit = await frames(120);
        const editCalls = vp.stats.drawCalls;
        await s.startPlay();
        await frames(30);
        const play = await frames(180);
        const ph = s.play.physics; let a = 0, b = 0;
        for (let i = 0; i < 60; i++) { let t0 = performance.now(); ph.step(1 / 60); a += performance.now() - t0; t0 = performance.now(); ph.syncToInstances(); b += performance.now() - t0; }
        const awake = [...ph.bodies.values()].filter((x) => !x.anchored && !x.body.isSleeping()).length;
        await s.stopPlay();
        return { parts: 3000, edit, editDrawCalls: editCalls, batches: vp.drawBatches, play, physicsStepMs: +(a / 60).toFixed(2), syncMs: +(b / 60).toFixed(2), awakeBodies: awake };
      })()`);
      console.log('BENCH ' + JSON.stringify(result));
      app.exit(0);
    } catch (e) {
      console.error('BENCH ERROR', e);
      app.exit(2);
    }
  });
}

module.exports = { runSmokeTest, runBenchmark };

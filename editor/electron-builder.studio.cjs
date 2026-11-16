// Forge Studio package. Ships the Forge Server binary as a resource so
// "Start local server" works without a separate install.
const base = require('./build/builder-base.cjs');
const serverExe = process.platform === 'win32' ? 'forge-server.exe' : 'forge-server';

module.exports = base({
  id: 'studio',
  name: 'Forge Studio',
  mode: 'studio',
  icon: 'build/icons/studio.png',
  artifact: 'ForgeStudio',
  extraResources: [{ from: `server/target/release/${serverExe}`, to: serverExe }],
});

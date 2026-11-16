// Exposes a minimal, explicit API to the renderer (Studio and Player).
const { contextBridge, ipcRenderer } = require('electron');

const modeArg = process.argv.find((a) => a.startsWith('--forge-mode='));

contextBridge.exposeInMainWorld('forgeNative', {
  isElectron: true,
  mode: modeArg ? modeArg.split('=')[1] : 'studio',
  openDialog: (kind, multi = false) => ipcRenderer.invoke('fs:openDialog', { kind, multi }),
  saveDialog: (kind, defaultName) => ipcRenderer.invoke('fs:saveDialog', { kind, defaultName }),
  readFile: (p) => ipcRenderer.invoke('fs:readFile', p),
  writeFile: (p, data) => ipcRenderer.invoke('fs:writeFile', p, data),
  gitRun: (opts) => ipcRenderer.invoke('git:run', opts),
  netRequest: (opts) => ipcRenderer.invoke('net:request', opts),
  pickFolder: (opts) => ipcRenderer.invoke('map:pickFolder', opts),
  readMapDir: (dir) => ipcRenderer.invoke('map:readDir', dir),
  writeMapFiles: (opts) => ipcRenderer.invoke('map:writeFiles', opts),
  autosaveWrite: (id, text) => ipcRenderer.invoke('autosave:write', id, text),
  autosaveList: () => ipcRenderer.invoke('autosave:list'),
  autosaveRemove: (id) => ipcRenderer.invoke('autosave:remove', id),
  libraryDir: () => ipcRenderer.invoke('library:dir'),
  libraryList: () => ipcRenderer.invoke('library:list'),
  libraryWriteMeta: (p, meta) => ipcRenderer.invoke('library:writeMeta', p, meta),
  libraryTrash: (p) => ipcRenderer.invoke('library:trash', p),
  libraryReveal: (p) => ipcRenderer.invoke('library:reveal', p),
  libraryUniquePath: (name) => ipcRenderer.invoke('library:uniquePath', name),
  languageList: () => ipcRenderer.invoke('lang:list'),
  languageOpenFolder: () => ipcRenderer.invoke('lang:openFolder'),
  serverStart: (port) => ipcRenderer.invoke('server:start', port),
  serverStop: () => ipcRenderer.invoke('server:stop'),
  serverStatus: () => ipcRenderer.invoke('server:status'),
  launchPlayer: () => ipcRenderer.invoke('app:launchPlayer'),
  setTitle: (t) => ipcRenderer.invoke('app:setTitle', t),
  ready: () => ipcRenderer.invoke('app:ready'),
  status: (t) => ipcRenderer.invoke('app:status', t),
  confirmClose: () => ipcRenderer.invoke('app:confirm-close'),
  onCloseRequested: (cb) => ipcRenderer.on('app:close-requested', () => cb()),
});

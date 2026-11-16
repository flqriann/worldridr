import { createRoot } from 'react-dom/client';
import './styles.css';
import './styles-shell.css';
import 'monaco-editor/editor/editor.main';
import { Studio } from './editor/Studio';
import './runtime/PlaySession'; // registers the play session factory
import { Root } from './ui/App';
import { initEngine } from './engine/engine';
import { initPhysics } from './runtime/Physics';
import { loadFolderLanguagePacks } from './i18n/folder';
import { native } from './project/ProjectIO';
import { t } from './i18n';

// The Rust core (terrain) and Rapier (physics) are WebAssembly modules that must be loaded first.
native?.status('Loading engine…');
const [, , langErrors] = await Promise.all([initEngine(), initPhysics(), loadFolderLanguagePacks()]);
native?.status('Starting Studio…');
const studio = new Studio();
(window as unknown as { forgeStudio: Studio }).forgeStudio = studio; // handy for debugging in DevTools

window.addEventListener('error', (e) => studio.output.error(`Unexpected error: ${e.message}`));
window.addEventListener('unhandledrejection', (e) => studio.output.error(`Unexpected error: ${(e.reason as Error)?.message ?? e.reason}`));

createRoot(document.getElementById('root')!).render(<Root studio={studio} />);
studio.output.system('WorldRidr Studio ready. Import a place (File > Import Place), open a map folder, or start a new project.');
for (const err of langErrors) studio.output.warn(t('Language pack skipped: {error}', { error: err }));
document.getElementById('boot-splash')?.classList.add('hide');
setTimeout(() => document.getElementById('boot-splash')?.remove(), 600);
native?.ready();
studio.refreshLocalServer().catch(() => {});
studio.checkRecovery().catch((e) => studio.output.warn(`Autosave recovery check failed: ${(e as Error).message}`));

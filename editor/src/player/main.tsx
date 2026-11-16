import { createRoot } from 'react-dom/client';
import '../styles.css';
import '../styles-shell.css';
import './player.css';
import { initEngine } from '../engine/engine';
import { initPhysics } from '../runtime/Physics';
import { loadFolderLanguagePacks } from '../i18n/folder';
import { native } from '../project/ProjectIO';
import { PlayerRoot } from './PlayerApp';

native?.status('Loading engine…');
await Promise.all([initEngine(), initPhysics(), loadFolderLanguagePacks()]);
createRoot(document.getElementById('root')!).render(<PlayerRoot />);
document.getElementById('boot-splash')?.classList.add('hide');
setTimeout(() => document.getElementById('boot-splash')?.remove(), 600);
native?.ready();

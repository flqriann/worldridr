import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// `base: './'` keeps all asset URLs relative so the build also works when
// Electron serves it through the custom app:// protocol.
export default defineConfig({
  base: './',
  plugins: [react()],
  build: {
    outDir: 'dist', chunkSizeWarningLimit: 6000, target: 'es2022',
    // two apps: Studio (index.html) and Player (player.html)
    rollupOptions: { input: { studio: 'index.html', player: 'player.html' } },
  },
  worker: { format: 'es' },
  optimizeDeps: { include: ['wasmoon'] },
});

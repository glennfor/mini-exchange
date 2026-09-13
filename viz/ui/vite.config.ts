import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

export default defineConfig({
  plugins: [react()],
  server: {
    // Not Vite's default 5173, which every other project on a developer's
    // machine is also using. strictPort so a collision is a loud error rather
    // than a silent move to another port -- which is how you end up
    // screenshotting somebody else's dashboard.
    port: 5199,
    strictPort: true,
  },
  build: {
    outDir: 'dist',
    // The dashboard is one page with a handful of canvases; a source map
    // costs nothing to ship and makes a production stack trace readable.
    sourcemap: true,
  },
});

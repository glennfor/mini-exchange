import { defineConfig } from 'vitest/config';

import react from '@vitejs/plugin-react';

// Two environments: the pure-logic tests (protocol parsing, formatting, the
// heatmap ring buffer) run in node, and the component tests run in jsdom with
// a stubbed WebSocket. The component tests are what verify the panels
// actually mount and react to frames -- browser screenshots are not something
// CI can depend on.
export default defineConfig({
  plugins: [react()],
  test: {
    environment: 'jsdom',
    include: ['src/**/*.test.ts', 'src/**/*.test.tsx'],
    setupFiles: ['./src/test-setup.ts'],
  },
});

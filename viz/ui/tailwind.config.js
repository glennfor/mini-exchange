/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{ts,tsx}'],
  theme: {
    extend: {
      colors: {
        // A trading window, not a consumer dashboard: near-black, low
        // chroma, and everything that is not data recedes.
        ink: {
          900: '#08090b',
          800: '#0e1013',
          700: '#15181d',
          600: '#1e222a',
          500: '#2a2f3a',
        },
        bid: '#2f9e6e',
        ask: '#c9524a',
        accent: '#d9a441',
        muted: '#6b7280',
      },
      fontFamily: {
        mono: ['ui-monospace', 'SFMono-Regular', 'Menlo', 'Consolas', 'monospace'],
      },
    },
  },
  plugins: [],
};

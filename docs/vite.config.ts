import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  // Relative base so the build works under any GitHub Pages repo path
  // (user.github.io/<repo>/) without hardcoding the repository name.
  base: './',
  build: { outDir: 'dist' },
});

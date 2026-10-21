import { defineConfig } from 'astro/config';

// A pure static build, deployed to GitHub Pages from this repository's
// gh-pages branch. The SDK's site is the root, so its base is '/'.
const base = process.env.BASE_PATH ?? '/PACE';

export default defineConfig({
  site: 'https://devansh0703.github.io/PACE',
  base,
  trailingSlash: 'ignore',
  build: {
    format: 'directory',
  },
});

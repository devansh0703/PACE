import { defineConfig } from 'astro/config';

// A pure static build, deployed to GitHub Pages from this repository's
// gh-pages branch. The analyzer's site is served from /analyzer/, so its
// base is that path, and the site's own links are relative to it.
const base = process.env.BASE_PATH ?? '/PACE/analyzer';

export default defineConfig({
  site: 'https://devansh0703.github.io/PACE',
  base,
  trailingSlash: 'ignore',
  build: {
    format: 'directory',
  },
});

import { defineConfig } from 'astro/config';

// One product, one site, deployed to GitHub Pages from the gh-pages branch.
// GitHub Pages serves a project site from /<repo>/, so the site's base is
// /PACE and every internal link is written relative to a page rather than to
// the domain root. A root-absolute asset URL would 404 here, which
// .github/workflows/pages.yml checks for after the build.
const base = process.env.BASE_PATH ?? '/PACE';

export default defineConfig({
  site: 'https://devansh0703.github.io/PACE',
  base,
  trailingSlash: 'ignore',
  build: {
    format: 'directory',
  },
});

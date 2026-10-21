import { defineConfig } from 'astro/config';

// The site is a pure static build. The base path is set for GitHub Pages
// project deployments; set BASE_PATH='/' for a custom domain.
const base = process.env.BASE_PATH ?? '/';

export default defineConfig({
  site: 'https://devansh0703.github.io',
  base,
  trailingSlash: 'ignore',
  build: {
    format: 'directory',
  },
});

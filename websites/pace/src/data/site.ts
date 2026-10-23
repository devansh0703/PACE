// The single source of truth for what the site says about the product.
//
// Every URL, address and identifier the site prints comes from here, so a
// stale or invented contact detail cannot survive in a page: there is no
// second copy of it to forget.

export const site = {
  name: 'PACE',
  short: 'PACE',
  product: 'PACE',
  tagline: 'Preemption-aware time attribution, in-process and privilege-free',
  description:
    'PACE splits every thread\u2019s wall time into the time it ran, the time the scheduler took the core away, and the time it was blocked — in-process, with no elevated privilege.',
  // The deployed site. GitHub Pages serves this repository's Pages artifact.
  url: 'https://devansh0703.github.io/PACE/',
  repo: 'https://github.com/devansh0703/PACE',
  issues: 'https://github.com/devansh0703/PACE/issues',
  discussions: 'https://github.com/devansh0703/PACE/discussions',
  contact: 'devanshraulo@gmail.com',
  author: 'Devansh Raulo',
  licenceSdk: 'BUSL-1.1',
  licenceAnalyzer: 'Apache-2.0',
  // The hosted analysis service. Set when the Render service is live; the
  // pages that use it render a "self-host instead" path while it is empty.
  service: 'https://pace-service.onrender.com',
  version: '0.1.2',
};

// The nav is one product's nav, not two products' navs.
export const nav = [
  { href: '/product/', label: 'Product' },
  { href: '/service/', label: 'Service' },
  { href: '/install/', label: 'Install' },
  { href: '/docs/', label: 'Docs' },
  { href: '/research/', label: 'Research' },
  { href: '/about/', label: 'About' },
];

// A link into this site, with the deployment base already applied.
//
// GitHub Pages serves this repository from /PACE/, so a bare "/product/"
// would resolve to the domain root and 404. import.meta.env.BASE_URL is
// whatever astro.config.mjs set (overridable with BASE_PATH), so every
// internal link is written once, here.
export function href(path = ''): string {
  const base = import.meta.env.BASE_URL.replace(/\/$/, '');
  return `${base}/${path.replace(/^\//, '')}`;
}

// The nav entry whose link matches the current page, for aria-current.
export function isCurrent(currentPath: string, target: string): boolean {
  const normalized = currentPath.replace(/\/$/, '') || '/';
  const base = import.meta.env.BASE_URL.replace(/\/$/, '');
  return normalized === `${base}${target}`;
}

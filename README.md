# Cressida website

The public Cressida homepage and user documentation, built with Astro and
Starlight. Static output is deployed to GitHub Pages.

## Local development

Use Node.js 24 and npm. The minimum supported Node version is 22.22.0.

```sh
npm ci
npm run dev
```

Open the local URL printed by Astro, normally `http://localhost:4321/`. The
homepage is `/`, documentation starts at `/docs/`, and unknown URLs use the
custom Cressida 404. Both slashless and trailing-slash URLs are accepted.

## Validate

```sh
npm run check
npm run build
npx --no-install playwright install chromium
npm run test:browser
```

To check the development server's missing-page handling as well:

```sh
TEST_SERVER=dev npm run test:browser -- --grep "unknown path"
```

`TEST_PORT` can select an unused port for the isolated test server. The default
is 4322. Tests start and stop their own server; they do not reuse a running
preview.

`check` runs Astro diagnostics and formatting checks. `build` also verifies
generated local links, assets, fragments, the search bundle, sitemap and 404.
Browser checks cover navigation, color persistence, documentation search, mobile
layout and recovery from missing URLs.

`npm run format` formats source files. `npm run preview` serves the production
output locally. Search is generated during the production build; use preview to
test search rather than the development server.

### Test a Pages subpath

Use the same variables for the build, preview and browser tests:

```sh
export SITE_URL=https://example.test
export BASE_PATH=/cressida-website/
npm run build
npm run test:browser
```

Without these variables, local development uses `/`. `SITE_URL` is the canonical
origin, not a URL with the repository path attached. `BASE_PATH` supplies that
path separately. The production workflow sets both from GitHub Pages itself.

## GitHub Pages setup

1. Put this project at the root of the website repository, with `main` as its
   deployment branch. If the default branch differs, update the push branch in
   `.github/workflows/pages.yml`.
2. In repository **Settings > Pages > Build and deployment**, select **GitHub
   Actions** as the source.
3. If using a custom domain, configure it in Pages settings and complete its DNS
   setup. The workflow reads the configured origin and base path, so neither
   needs to be hardcoded in the source.
4. A push to `main`, or **Deploy GitHub Pages > Run workflow**, validates the
   site, builds it, uploads `dist/` and deploys it to the `github-pages`
   environment.

Pull requests run the checks only. Validation builds at both `/` and a
repository subpath and exercises the browser in each configuration. The
deployment workflow waits for those checks. Build jobs have read access; only
the deploy job gets Pages write and identity-token permissions. Actions are
pinned to immutable revisions, and Dependabot maintains action and npm update
proposals.

GitHub Pages serves the generated `404.html` for missing URLs inside this site's
hosting scope. The same custom page is used by Astro dev and preview. No SPA
rewrite or client-side router is required.

## Content

- `src/pages/index.astro`: custom product homepage.
- `src/pages/404.astro`: recovery page.
- `src/content/docs/docs/`: user guides and development reference.
- `astro.config.mjs`: Starlight sidebar and Pages-aware configuration.
- `src/styles/`: shared palette tokens and site styles.
- `public/images/`: actual offline native-client captures.
- `scripts/check-site.mjs`: checks against the built output.

Keep homepage claims and documentation aligned with current client behavior.
Check installation commands, settings labels and supported actions against the
client source before changing them. Keep planned features clearly unavailable.
Write for users and do not use em dashes.

To add a guide, create a Markdown file with `title` and `description`
frontmatter and add its slug to the sidebar. Use relative links in Markdown and
`import.meta.env.BASE_URL` in Astro pages so links also work under a Pages
subpath.

## Assets and license

The Cressida logo is copied from the client's original SVG without changing its
paths, gradients or layer order. `public/cressida-logo.svg` is the served copy;
`src/assets/cressida-logo.svg` is Starlight's build-time copy. Keep them
identical.

The two native-window screenshots use generated offline demo data, not a live
account. They were captured from the client on 2026-10-21. Preserve that
distinction in captions and use sample data for future captures.

Project content and artwork retain the supplied [MIT license](./LICENSE). Astro,
Starlight and other dependencies retain their own licenses.

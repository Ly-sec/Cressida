# Cressida website

The public Cressida homepage and documentation renderer, built with Astro and
Starlight and hosted on GitHub Pages. This source lives on the **website**
branch of the Cressida repository. Editable documentation stays on **main**.

## Where content lives

| Branch    | Content                                                                                                           |
| --------- | ----------------------------------------------------------------------------------------------------------------- |
| `main`    | Client source, `docs/user/` Markdown guides, `docs/sidebar.json` navigation and the small documentation workflow. |
| `website` | Astro/Starlight, homepage, 404, styles, assets and reusable build workflows.                                      |

The build imports docs into the ignored `.docs/` directory. Never edit or commit
that generated copy. There are no synchronization commits or documentation
copies to keep current across branches.

## Local development

Use Node.js 24 and npm; the minimum Node version is 22.22.0. Keep a separate
local checkout of `main` available and point to its `docs/` directory:

```sh
export CRESSIDA_DOCS_DIR=/absolute/path/to/client/docs
npm ci
npm run dev
```

Open the printed local URL, normally `http://localhost:4321/`. Documentation
starts at `/docs/`. Unknown URLs with or without a trailing slash use the
Cressida 404.

`dev`, `check` and `build` import current docs automatically. Source docs are
never modified by importing. Missing or invalid input fails rather than silently
building old content. To refresh after editing docs during a dev session, run
`npm run docs:sync` and restart the server, including after sidebar changes.

## Validate

With `CRESSIDA_DOCS_DIR` still set:

```sh
npm run check
npm run build
npx --no-install playwright install chromium
npm run test:browser
TEST_SERVER=dev npm run test:browser -- --grep "unknown path"
```

`check` runs import/link helper tests, source-doc formatting, Astro diagnostics
and website formatting. `build` verifies generated local links, assets,
fragments, search, sitemap and the 404. Browser tests cover navigation, search,
color persistence, mobile layout and missing URLs. `TEST_PORT` selects a
test-server port; the default is 4322. Tests own their foreground server.

Use `npm run format` for website source and `npm run docs:format` for the main
checkout's documentation. The latter intentionally edits the source docs
directory. Guide filenames and `.md` links stay readable on GitHub. The remark
link plugin converts document links during rendering, preserving anchors and
code examples.

### Repository-subpath testing

Use the same settings for build, preview and browser tests:

```sh
export SITE_URL=https://example.test
export BASE_PATH=/cressida-website/
npm run build
npm run test:browser
```

Local development defaults to `/`. `SITE_URL` is the canonical origin and
`BASE_PATH` is its separate path prefix. The production workflow reads both from
GitHub Pages. `npm run preview` serves the built output. Search requires a
production build rather than the development server.

## GitHub Pages setup

1. Keep the main-branch documentation and its `.github/workflows/docs.yml`
   wrapper on `main`. Keep this website and its reusable workflows on `website`.
2. In **Settings > Pages > Build and deployment**, choose **GitHub Actions** as
   **Source**. Do not choose **Deploy from a branch** for the Astro source.
3. In **Settings > Environments > github-pages**, allow deployments from both
   `main` and `website`, while keeping any desired reviewer protections.
4. Configure your custom domain and DNS in Pages settings if desired. The build
   reads the configured origin and base path automatically.
5. Once both branches contain their parts, run **Documentation website**
   manually from the Actions tab for the initial deployment.

No personal access token, external server or branch-writing bot is needed. Read
jobs use `contents: read`; only the deployment job needs Pages write and an
identity token. Checkouts do not persist credentials. Actions and npm
dependencies are pinned and Dependabot maintains update proposals.

### Automatic triggers

| Change                               | Result                                                                    |
| ------------------------------------ | ------------------------------------------------------------------------- |
| Push to `main` changing `docs/**`    | Build with that exact docs revision and the current website, then deploy. |
| Other push to `main`                 | No website build or deployment.                                           |
| Documentation PR into `main`         | Validate the PR's merged docs against the website branch; no deployment.  |
| Any push to `website`                | Build that website revision with current `main` docs and deploy.          |
| PR into `website`                    | Validate the proposed website with `main` docs; no deployment.            |
| Manual **Documentation website** run | Resolve both branches and deploy after checks.                            |

Deployment resolves both revisions once before checking them. The final build
uses the same revisions. Root and repository-subpath configurations are checked,
including production search and custom 404 behavior in dev and preview.

### A build mentions Jekyll or invalid YAML in an Astro page

GitHub is trying to run its default branch-publishing pipeline. Astro's `---`
blocks contain JavaScript, which Jekyll interprets as YAML. Change Pages
**Source** to **GitHub Actions**, then run **Documentation website**. Changing
the Astro frontmatter or serving its raw source will not build the website.

The `website` branch contains source; the workflow publishes compiled `dist/`.
`public/.nojekyll` is included in that static output. GitHub Pages serves its
`404.html` for missing URLs within this site's hosting scope. No SPA rewrite is
needed.

## Files

- `src/pages/index.astro`: product homepage.
- `src/pages/404.astro`: recovery page.
- `src/content.config.ts`: generated documentation loader and Starlight schema.
- `astro.config.mjs`: imported sidebar and Pages-aware configuration.
- `scripts/sync-docs.mjs`: validated import of docs, assets and navigation.
- `scripts/docs-links.mjs`: repository Markdown links to public routes.
- `scripts/check-site.mjs`: checks against built HTML.
- `src/styles/` and `public/`: website styles and assets.

New guides and their sidebar entries belong under `docs/` on `main`. Preserve
existing public routes when reorganizing documentation. Keep homepage claims
accurate when the client changes; do not use em dashes.

## Assets and license

The original client logo is preserved without changing paths, gradients or layer
order. Keep `public/cressida-logo.svg` and `src/assets/cressida-logo.svg`
identical. Native-window screenshots use offline sample data, captured on
2026-10-21. Use sample data for future captures and keep that distinction in
captions.

Project content and artwork retain the supplied [MIT license](./LICENSE).
Dependencies retain their own licenses.

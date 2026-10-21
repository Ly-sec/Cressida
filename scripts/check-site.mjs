import { readFile, readdir, access } from "node:fs/promises";
import path from "node:path";

// Verify the generated output, including deployment prefixes and link fragments.
const output = path.resolve("dist");
const origin = new URL(process.env.SITE_URL || "https://ly-sec.github.io")
  .origin;
const base =
  `/${(process.env.BASE_PATH || "/").split("/").filter(Boolean).join("/")}`.replace(
    /\/?$/,
    "/",
  );
const files = await readdir(output, { recursive: true });
const htmlFiles = files.filter((file) => file.endsWith(".html"));
const documents = new Map();
const errors = [];
const decode = (value) =>
  value
    .replaceAll("&amp;", "&")
    .replaceAll("&#39;", "'")
    .replaceAll("&quot;", '"');
for (const file of htmlFiles) {
  const html = await readFile(path.join(output, file), "utf8");
  documents.set(file, {
    html,
    ids: new Set(
      [...html.matchAll(/\bid="([^"]*)"/g)].map((match) => decode(match[1])),
    ),
  });
}
let checked = 0;
for (const [file, { html }] of documents) {
  const current = new URL(base + file.replace(/index\.html$/, ""), origin);
  if (!/<html[^>]*\blang="en"/.test(html))
    errors.push(`${file}: missing document language`);
  if (!/<title>[^<]+<\/title>/.test(html))
    errors.push(`${file}: missing page title`);
  for (const match of html.matchAll(/\b(?:href|src)="([^"]+)"/g)) {
    const value = decode(match[1]);
    if (/^(?:data:|mailto:|tel:|javascript:)/.test(value)) continue;
    const link = new URL(value, current);
    if (link.origin !== origin) continue;
    if (!link.pathname.startsWith(base)) {
      errors.push(`${file}: link escapes deployment prefix: ${value}`);
      continue;
    }
    const local = decodeURIComponent(link.pathname.slice(base.length));
    const candidates =
      local.endsWith("/") || !local
        ? [`${local}index.html`]
        : [local, `${local}/index.html`];
    let target;
    for (const candidate of candidates) {
      if (!files.includes(candidate)) continue;
      target = candidate;
      break;
    }
    if (!target) {
      errors.push(`${file}: missing local target: ${value}`);
      continue;
    }
    const fragment = decodeURIComponent(link.hash.slice(1));
    if (
      fragment &&
      documents.has(target) &&
      !documents.get(target).ids.has(fragment)
    ) {
      errors.push(`${file}: missing fragment: ${value}`);
    }
    checked++;
  }
}
await access(path.join(output, "404.html"));
await access(path.join(output, "pagefind/pagefind.js"));
await access(path.join(output, "sitemap-index.xml"));
if (errors.length)
  throw new Error(`Generated-site checks failed:\n${errors.join("\n")}`);
console.log(
  `Verified ${htmlFiles.length} pages and ${checked} local links/assets at ${base}.`,
);

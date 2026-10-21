import { existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const generated = fileURLToPath(
  new URL("../.docs/content/docs/", import.meta.url),
);

function route(file, root) {
  return path
    .relative(root, file)
    .split(path.sep)
    .join("/")
    .replace(/(^|\/)index\.mdx?$/, "$1")
    .replace(/\.mdx?$/, "")
    .replace(/\/$/, "");
}

export function documentationLink(url, file, root = generated) {
  if (/^(?:[a-z][a-z0-9+.-]*:|\/|#)/i.test(url)) return url;
  const match = /^([^?#]+)\.mdx?([?#].*)?$/.exec(url);
  if (!match) return url;
  const filename = decodeURIComponent(url.split(/[?#]/, 1)[0]);
  const target = path.resolve(path.dirname(file), filename);
  if (
    !target.startsWith(path.resolve(root) + path.sep) ||
    !existsSync(target)
  ) {
    throw new Error(
      `Documentation link has no page: ${url} in ${path.basename(file)}`,
    );
  }
  const from = route(file, root);
  const to = route(target, root);
  const relative = path.posix.relative(from || ".", to || ".") || ".";
  return `${relative}/` + (match[2] || "");
}

export default function docsLinks(options = {}) {
  const root = path.resolve(options.root || generated) + path.sep;
  return (tree, file) => {
    if (!file.path || !path.resolve(file.path).startsWith(root)) return;
    const visit = (node) => {
      if (
        (node.type === "link" || node.type === "definition") &&
        typeof node.url === "string"
      ) {
        node.url = documentationLink(node.url, file.path, root);
      }
      for (const child of node.children || []) visit(child);
    };
    visit(tree);
  };
}

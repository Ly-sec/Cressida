import {
  cp,
  lstat,
  mkdir,
  mkdtemp,
  readFile,
  readdir,
  rename,
  rm,
} from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

export const siteRoot = fileURLToPath(new URL("../", import.meta.url));

export function docsDirectory() {
  const directory = process.env.CRESSIDA_DOCS_DIR;
  if (!directory) {
    throw new Error(
      "Set CRESSIDA_DOCS_DIR to the main checkout's docs/ directory. See README.md.",
    );
  }
  return path.resolve(directory);
}

async function sourceFiles(directory) {
  const files = [];
  const visit = async (current) => {
    const stat = await lstat(current);
    if (stat.isSymbolicLink())
      throw new Error(`Documentation must not contain symlinks: ${current}`);
    if (stat.isDirectory()) {
      for (const entry of await readdir(current))
        await visit(path.join(current, entry));
    } else if (stat.isFile()) {
      files.push(current);
    } else {
      throw new Error(`Unsupported documentation file: ${current}`);
    }
  };
  await visit(directory);
  return files;
}

export async function documentationFiles(source) {
  const root = path.resolve(source);
  if (!(await lstat(root)).isDirectory())
    throw new Error("Documentation source must be a real directory.");
  const files = await sourceFiles(path.join(root, "user"));
  const index = path.join(root, "user/index.md");
  if (!files.includes(index))
    throw new Error(
      "Documentation needs user/index.md; existing generated docs were kept.",
    );
  const sidebar = path.join(root, "sidebar.json");
  if (
    !(await lstat(sidebar)).isFile() ||
    (await lstat(sidebar)).isSymbolicLink()
  ) {
    throw new Error("sidebar.json must be a regular file.");
  }
  const navigation = JSON.parse(await readFile(sidebar, "utf8"));
  if (!Array.isArray(navigation) || !navigation.length)
    throw new Error("sidebar.json must contain a nonempty sidebar array.");
  return { files, sidebar };
}

export async function syncDocs(source, destination) {
  source = path.resolve(source);
  destination = path.resolve(destination);
  if (
    destination === source ||
    destination.startsWith(source + path.sep) ||
    source.startsWith(destination + path.sep)
  ) {
    throw new Error(
      "Documentation source and generated output must be separate directories.",
    );
  }
  const { files, sidebar } = await documentationFiles(source);
  // Validate before touching the last successful import, then replace the full cache.
  await mkdir(path.dirname(destination), { recursive: true });
  const temporary = await mkdtemp(
    path.join(path.dirname(destination), ".docs-import-"),
  );
  const backup = `${temporary}-previous`;
  let previous = false;
  try {
    const content = path.join(temporary, "content/docs");
    await mkdir(content, { recursive: true });
    for (const file of files) {
      const target = path.join(
        content,
        path.relative(path.join(source, "user"), file),
      );
      await mkdir(path.dirname(target), { recursive: true });
      await cp(file, target, { dereference: false });
    }
    await cp(sidebar, path.join(temporary, "sidebar.json"));
    try {
      const stat = await lstat(destination);
      if (!stat.isDirectory() || stat.isSymbolicLink())
        throw new Error("Generated docs output must be a real directory.");
      await rename(destination, backup);
      previous = true;
    } catch (error) {
      if (error.code !== "ENOENT") throw error;
    }
    try {
      await rename(temporary, destination);
    } catch (error) {
      if (previous) await rename(backup, destination);
      throw error;
    }
    if (previous) await rm(backup, { recursive: true });
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
  return files.filter((file) => /\.mdx?$/.test(file)).length;
}

if (
  process.argv[1] &&
  path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)
) {
  const count = await syncDocs(docsDirectory(), path.join(siteRoot, ".docs"));
  console.log(`Imported ${count} documentation pages from main into .docs/.`);
}

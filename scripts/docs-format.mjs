import { readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { check, format, resolveConfig } from "prettier";
import { docsDirectory, documentationFiles, siteRoot } from "./sync-docs.mjs";

const source = docsDirectory();
const { files, sidebar } = await documentationFiles(source);
const options = await resolveConfig(path.join(siteRoot, "README.md"));
const write = process.argv.includes("--write");
let failed = false;
for (const file of [
  ...files.filter((file) => /\.mdx?$/.test(file)),
  sidebar,
  path.join(source, "README.md"),
]) {
  const content = await readFile(file, "utf8");
  const settings = { ...options, filepath: file };
  if (write) await writeFile(file, await format(content, settings));
  else if (!(await check(content, settings))) {
    console.error(
      `Documentation formatting: ${path.relative(source, file)}. Run npm run docs:format.`,
    );
    failed = true;
  }
}
if (failed) process.exitCode = 1;

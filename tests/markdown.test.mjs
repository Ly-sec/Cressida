import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtemp, writeFile, rm } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { unified } from "@astrojs/markdown-remark";
import docsLinks from "../scripts/docs-links.mjs";

test("real Markdown rendering rewrites page links and definitions while preserving code examples", async (t) => {
  const root = await mkdtemp(path.join(os.tmpdir(), "cressida-markdown-test-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const index = path.join(root, "index.md");
  await writeFile(index, "Overview.");
  await writeFile(path.join(root, "chat.md"), "Chat.");
  const renderer = await unified({
    remarkPlugins: [[docsLinks, { root }]],
    smartypants: false,
  }).createRenderer({ syntaxHighlight: false });
  const result = await renderer.render(
    [
      "[Chat](chat.md#reply)",
      "",
      "[Reference][guide]",
      "",
      "[guide]: chat.md",
      "",
      "`[Example](missing.md)`",
      "",
      "```md",
      "[Example](missing.md)",
      "```",
    ].join("\n"),
    { fileURL: pathToFileURL(index) },
  );
  assert.match(result.code, /href="chat\/#reply"/);
  assert.match(result.code, /href="chat\/"/);
  assert.match(result.code, /<code>\[Example\]\(missing.md\)<\/code>/);
  assert.match(result.code, /<pre[^>]*><code[^>]*>\[Example\]\(missing.md\)/);
  t.mock.method(console, "error", () => {});
  await assert.rejects(
    renderer.render("[Broken](missing.md)", { fileURL: pathToFileURL(index) }),
    /no page/,
  );
});

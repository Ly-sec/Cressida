import { test } from "node:test";
import assert from "node:assert/strict";
import {
  mkdtemp,
  mkdir,
  writeFile,
  readFile,
  rm,
  symlink,
  access,
} from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { syncDocs } from "../scripts/sync-docs.mjs";
import { documentationLink } from "../scripts/docs-links.mjs";

async function fixture(t) {
  const root = await mkdtemp(path.join(os.tmpdir(), "cressida-docs-test-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const source = path.join(root, "source");
  const destination = path.join(root, "generated");
  await mkdir(path.join(source, "user/assets"), { recursive: true });
  await writeFile(path.join(source, "sidebar.json"), '[{"slug":"docs"}]');
  await writeFile(
    path.join(source, "user/index.md"),
    "---\ntitle: Overview\n---\n[Chat](chat.md)\n",
  );
  await writeFile(
    path.join(source, "user/chat.md"),
    "---\ntitle: Chat\n---\nText.\n",
  );
  await writeFile(path.join(source, "user/assets/example.svg"), "<svg/>");
  return { root, source, destination };
}

test("imports docs, nested assets and navigation without rewriting source; removes stale pages", async (t) => {
  const { source, destination } = await fixture(t);
  assert.equal(await syncDocs(source, destination), 2);
  const imported = path.join(destination, "content/docs/index.md");
  assert.equal(
    await readFile(imported, "utf8"),
    await readFile(path.join(source, "user/index.md"), "utf8"),
  );
  assert.equal(
    await readFile(
      path.join(destination, "content/docs/assets/example.svg"),
      "utf8",
    ),
    "<svg/>",
  );
  await rm(path.join(source, "user/chat.md"));
  assert.equal(await syncDocs(source, destination), 1);
  await assert.rejects(access(path.join(destination, "content/docs/chat.md")));
});

test("invalid or missing source keeps the previous successful import", async (t) => {
  const { source, destination } = await fixture(t);
  await syncDocs(source, destination);
  await writeFile(path.join(source, "sidebar.json"), "{");
  await assert.rejects(syncDocs(source, destination));
  await access(path.join(destination, "content/docs/chat.md"));
  await writeFile(path.join(source, "sidebar.json"), '[{"slug":"docs"}]');
  await rm(path.join(source, "user/index.md"));
  await assert.rejects(syncDocs(source, destination), /index.md/);
  await access(path.join(destination, "content/docs/index.md"));
});

test("rejects symlinks and overlapping directories before replacing output", async (t) => {
  const { root, source, destination } = await fixture(t);
  await syncDocs(source, destination);
  await symlink(
    path.join(source, "sidebar.json"),
    path.join(source, "user/secret.md"),
  );
  await assert.rejects(syncDocs(source, destination), /symlinks/);
  await assert.rejects(
    syncDocs(source, path.join(source, "output")),
    /separate/,
  );
  const alias = path.join(root, "alias");
  await symlink(destination, alias);
  await rm(path.join(source, "user/secret.md"));
  await assert.rejects(syncDocs(source, alias), /real directory/);
  await access(path.join(destination, "content/docs/chat.md"));
});

test("converts repository links to website routes, preserving anchors and index pages", async (t) => {
  const { source } = await fixture(t);
  const root = path.join(source, "user");
  assert.equal(
    documentationLink("chat.md#reply", path.join(root, "index.md"), root),
    "chat/#reply",
  );
  assert.equal(
    documentationLink("index.md", path.join(root, "chat.md"), root),
    "../",
  );
  assert.equal(
    documentationLink("chat.md#reply", path.join(root, "chat.md"), root),
    "./#reply",
  );
  await mkdir(path.join(root, "guides"));
  await writeFile(path.join(root, "guides/index.md"), "Nested guide.");
  assert.equal(
    documentationLink("../chat.md", path.join(root, "guides/index.md"), root),
    "../chat/",
  );
  assert.equal(
    documentationLink(
      "https://example.com/readme.md",
      path.join(root, "chat.md"),
      root,
    ),
    "https://example.com/readme.md",
  );
  assert.equal(
    documentationLink("./assets/example.svg", path.join(root, "chat.md"), root),
    "./assets/example.svg",
  );
  assert.throws(
    () => documentationLink("missing.md", path.join(root, "chat.md"), root),
    /no page/,
  );
  assert.throws(
    () => documentationLink("../sidebar.md", path.join(root, "chat.md"), root),
    /no page/,
  );
});

import { readFileSync } from "node:fs";
import { unified } from "@astrojs/markdown-remark";
import docsLinks from "./scripts/docs-links.mjs";
import { defineConfig } from "astro/config";
import starlight from "@astrojs/starlight";
import sitemap from "@astrojs/sitemap";

const base =
  `/${(process.env.BASE_PATH || "/").split("/").filter(Boolean).join("/")}`.replace(
    /\/?$/,
    "/",
  );

export default defineConfig({
  site: process.env.SITE_URL || "https://ly-sec.github.io",
  base,
  // Accept both forms so unknown slashless paths reach our custom 404 in dev too.
  trailingSlash: "ignore",
  markdown: {
    processor: unified({ remarkPlugins: [docsLinks], smartypants: false }),
  },
  integrations: [
    starlight({
      title: "Cressida",
      description:
        "A native chat client for Linux and Wayland. Installation, everyday use and customization.",
      logo: { src: "./src/assets/cressida-logo.svg" },
      favicon: "/cressida-logo.svg",
      disable404Route: true,
      social: [
        {
          icon: "github",
          label: "Cressida on GitHub",
          href: "https://github.com/Ly-sec/Cressida",
        },
      ],
      customCss: ["./src/styles/docs.css"],
      sidebar: JSON.parse(
        readFileSync(new URL("./.docs/sidebar.json", import.meta.url), "utf8"),
      ),
    }),
    sitemap(),
  ],
});

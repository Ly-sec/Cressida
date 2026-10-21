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
      sidebar: [
        {
          label: "Start here",
          items: [
            { slug: "docs" },
            { slug: "docs/installation" },
            { slug: "docs/manual-installation" },
            { slug: "docs/sign-in" },
          ],
        },
        {
          label: "Using Cressida",
          items: [
            { slug: "docs/chat" },
            { slug: "docs/direct-messages" },
            { slug: "docs/servers-and-threads" },
            { slug: "docs/media" },
            { slug: "docs/desktop" },
            { slug: "docs/keyboard-shortcuts" },
          ],
        },
        {
          label: "Make it yours",
          items: [{ slug: "docs/appearance" }, { slug: "docs/custom-themes" }],
        },
        {
          label: "Reference and help",
          items: [
            { slug: "docs/command-line" },
            { slug: "docs/files-and-data" },
            { slug: "docs/troubleshooting" },
            { slug: "docs/faq" },
          ],
        },
        {
          label: "Development",
          collapsed: true,
          items: [
            { slug: "docs/building" },
            { slug: "docs/contributing" },
            { slug: "docs/packaging" },
          ],
        },
      ],
    }),
    sitemap(),
  ],
});

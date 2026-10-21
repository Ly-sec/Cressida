import { glob } from "astro/loaders";
import { defineCollection } from "astro:content";
import { i18nLoader } from "@astrojs/starlight/loaders";
import { docsSchema, i18nSchema } from "@astrojs/starlight/schema";

export const collections = {
  docs: defineCollection({
    loader: glob({ base: "./.docs/content", pattern: "**/[^_]*.{md,mdx}" }),
    schema: docsSchema(),
  }),
  i18n: defineCollection({ loader: i18nLoader(), schema: i18nSchema() }),
};

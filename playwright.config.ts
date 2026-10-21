import { defineConfig, devices } from "@playwright/test";

const base =
  `/${(process.env.BASE_PATH || "/").split("/").filter(Boolean).join("/")}`.replace(
    /\/?$/,
    "/",
  );
const port = Number(process.env.TEST_PORT || 4322);
const server = process.env.TEST_SERVER === "dev" ? "dev" : "preview";

export default defineConfig({
  testDir: "./tests",
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 1 : 0,
  reporter: "list",
  use: {
    baseURL: `http://127.0.0.1:${port}${base}`,
    trace: "retain-on-failure",
  },
  projects: [{ name: "chromium", use: { ...devices["Desktop Chrome"] } }],
  webServer: {
    command: `npm run ${server} -- --ignore-lock --host 127.0.0.1 --port ${port}`,
    url: `http://127.0.0.1:${port}${base}`,
    reuseExistingServer: false,
  },
});

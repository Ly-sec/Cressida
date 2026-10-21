import { test, expect } from "@playwright/test";

test("homepage links lead to the install guide and docs", async ({ page }) => {
  const failures: string[] = [];
  page.on("pageerror", (error) => failures.push(error.message));
  page.on("response", (response) => {
    if (response.status() >= 400)
      failures.push(`${response.status()} ${response.url()}`);
  });
  await page.goto("./");
  await expect(page.getByRole("heading", { level: 1 })).toContainText(
    "Your conversations.",
  );
  await expect(page.locator(".hero-description")).toContainText("native");
  await page.getByRole("link", { name: "Get started" }).click();
  await expect(page.getByRole("heading", { level: 1 })).toHaveText(
    "Installation",
  );
  await page
    .getByRole("link", { name: "Manual installation", exact: true })
    .first()
    .click();
  await expect(page.getByRole("heading", { level: 1 })).toHaveText(
    "Manual installation",
  );
  await expect(page.locator("main")).toContainText("meson setup");
  expect(failures).toEqual([]);
});

test("color preference carries between the homepage and documentation", async ({
  page,
}) => {
  await page.goto("./");
  const initial = await page.locator("html").getAttribute("data-theme");
  await page.getByRole("button", { name: /Switch to .* theme/ }).click();
  const chosen = initial === "dark" ? "light" : "dark";
  await expect(page.locator("html")).toHaveAttribute("data-theme", chosen);
  await page.getByRole("link", { name: "Read the docs" }).click();
  await expect(page.locator("html")).toHaveAttribute("data-theme", chosen);
  await page.reload();
  await expect(page.locator("html")).toHaveAttribute("data-theme", chosen);
});

test("built documentation search returns and opens a real page", async ({
  page,
}) => {
  await page.goto("docs/");
  await page.getByRole("button", { name: "Search", exact: true }).click();
  await page.locator("#starlight__search input").fill("randomize");
  const result = page
    .locator(".pagefind-ui__result-link")
    .filter({ hasText: "Images and video" })
    .first();
  await expect(result).toBeVisible();
  await result.click();
  await expect(page.getByRole("heading", { level: 1 })).toHaveText(
    "Images and video",
  );
});

test("mobile navigation and content fit the viewport", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto("./");
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
  await page.getByRole("link", { name: "Docs", exact: true }).click();
  await page.getByRole("button", { name: "Menu", exact: true }).click();
  await page
    .getByRole("link", { name: "Custom themes", exact: true })
    .first()
    .click();
  await expect(page.getByRole("heading", { level: 1 })).toHaveText(
    "Custom themes",
  );
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
});

for (const missing of [
  "awdmkl",
  "awdmkl/",
  "docs/not-a-page",
  "docs/not-a-page/",
]) {
  test(`unknown path ${missing} uses our 404 with working recovery`, async ({
    page,
  }) => {
    const response = await page.goto(missing);
    expect(response?.status()).toBe(404);
    await expect(page.getByRole("heading", { level: 1 })).toHaveText(
      "A little out of orbit.",
    );
    await page.getByRole("link", { name: "Browse the docs" }).click();
    await expect(page.getByRole("heading", { level: 1 })).toHaveText(
      "Welcome to Cressida",
    );
  });
}

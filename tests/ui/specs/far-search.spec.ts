import { test, expect } from "@playwright/test";
import { robotState } from "../lib/fake-robot";

// photonvision-55: the far-tag search's switch and status in Settings > Robot state. With no tag in
// view (the bench) no camera has a good view, so it's searching; switched off, it stops at once.

async function counters(request: import("@playwright/test").APIRequestContext) {
  return (await (await request.get("/api/robotState")).json()) as Record<string, number | boolean>;
}

test("far-tag search: status, off stops it, on again", async ({ page, request }) => {
  const before = await robotState(request);
  test.skip(before.robotConnected, "a robot is connected");
  const card = page.locator(".v-card").filter({ has: page.locator(".v-card-title", { hasText: /^Robot state$/ }) });
  const toggle = card.locator('[data-pv-control="switch"][data-pv-label="Far-tag search when the pose is weak"] input');
  const status = card.getByTestId("far-search-status");
  try {
    await page.goto("/#/settings");
    if (!(await toggle.isChecked())) await toggle.check();
    await expect.poll(async () => (await counters(request)).farSearch).toBe(true);
    const start = await counters(request);
    if (start.farSearchStarved) {
      // Searching: full-size searches go up, and the status says so.
      await expect.poll(async () => (await counters(request)).farSearchSweeps as number, { timeout: 5_000 }).toBeGreaterThan(start.farSearchSweeps as number);
      await expect(status).toContainText("Searching now");
    } else {
      await expect(status).toContainText("Waiting: a camera has a good view");
    }
    await card.screenshot({ path: test.info().outputPath("far-search-card.png") });

    await toggle.uncheck();
    await expect.poll(async () => (await counters(request)).farSearch).toBe(false);
    await expect(status).toContainText("Off:");
    const off = (await counters(request)).farSearchSweeps as number;
    await page.waitForTimeout(2_000);
    expect((await counters(request)).farSearchSweeps, "no full-size searches while off").toBe(off);
    await expect.poll(async () => (await counters(request)).farSearchStarved).toBe(false);
  } finally {
    await request.post("/api/robotState", { data: { farSearch: before.farSearch, farSweepsPerSecond: before.farSweepsPerSecond } });
  }
});

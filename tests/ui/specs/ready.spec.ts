import { test, expect } from "@playwright/test";
import { uiState } from "../lib/pv";

// photonvision-52: the Match Ready page shows health-check.sh's verdict and checks, and a live tile
// per camera; "Check again" runs the checks again.

test("Match Ready: verdict, a live tile per camera, Check again", async ({ page, request }) => {
  await page.goto("/#/ready");
  const verdict = page.getByTestId("ready-verdict");
  await expect(verdict).toHaveText(/^(READY|NOT READY)/, { timeout: 70_000 });

  // The same verdict as the endpoint (the page's run is reused for 3 s, or a fresh one agrees).
  const api = await (await request.get("/api/healthCheck", { timeout: 70_000 })).json();
  expect((await verdict.innerText()).split(":")[0].split(" (")[0]).toBe(api.verdict.split(":")[0].split(" (")[0]);
  expect(api.sections.map((s: { name: string }) => s.name)).toEqual(
    expect.arrayContaining(["PhotonVision", "Cameras", "Robot connection", "System"])
  );

  for (const c of await uiState(request)) {
    const tile = page.getByTestId(`ready-camera-${c.nickname}`);
    await expect(tile).toBeVisible();
    await expect(tile).toContainText(`${c.currentPipelineIndex}: ${c.pipelineNicknames[c.currentPipelineIndex]}`);
    // Live frames: a non-zero fps within a few seconds.
    await expect
      .poll(async () => parseInt((/(\d+)\s*fps/.exec(await tile.innerText()) ?? [])[1] ?? "0"), { timeout: 10_000, message: c.nickname })
      .toBeGreaterThan(0);
  }
  await page.screenshot({ path: test.info().outputPath("match-ready.png"), fullPage: true });

  // A run within 3 s of the last is reused, so wait that out; then the page's own request must
  // bring a newer run.
  const before = api.ranAt as number;
  await page.waitForTimeout(3_500);
  const [response] = await Promise.all([
    page.waitForResponse((r) => r.url().includes("/api/healthCheck"), { timeout: 70_000 }),
    page.getByTestId("ready-recheck").click()
  ]);
  expect((await response.json()).ranAt).toBeGreaterThan(before);
  await expect(page.getByText(/Checked just now/)).toBeVisible();
});

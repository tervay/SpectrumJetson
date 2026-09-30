import { test, expect } from "@playwright/test";
import { FakeRobot, JETSON, robotState } from "../lib/fake-robot";
import { Dashboard } from "../lib/pv";
import { pickCamera } from "../lib/fixtures";

// photonvision-49/50: with a (fake) robot connected and disabled the cameras idle at 30 fps; the
// dashboard says so and its "Full speed" button, or Settings > Robot state, turns idling off.
// Needs the Jetson over SSH to run tests/fake-robot there (run.sh sets PV_UI_JETSON).

async function shownFps(dash: Dashboard): Promise<number> {
  const text = await dash.page.locator(".v-card-title .v-chip").first().innerText();
  return parseInt(/(\d+)\s*FPS/.exec(text)?.[1] ?? "0");
}

test("idle while disabled, and the Full speed / Settings switches", async ({ page, request }) => {
  test.skip(!JETSON, "needs PV_UI_JETSON (tests/ui/run.sh sets it) to start the fake robot");
  const before = await robotState(request);
  test.skip(before.robotConnected, "a robot is already connected");
  const camera = await pickCamera(request);

  // 50 s disabled at most; stopped early below.
  const robot = await FakeRobot.start(request, "disabled:50");
  try {
    if (!(await robotState(request)).idleWhileDisabled) {
      await request.post("/api/robotState", { data: { idleWhileDisabled: true } });
    }
    await expect.poll(async () => (await robotState(request)).idleNow).toBe(true);

    const dash = new Dashboard(page);
    await dash.open();
    await dash.selectCamera(camera);
    const notice = page.getByTestId("idle-notice");
    await expect(notice).toBeVisible({ timeout: 10_000 });
    await expect(notice).toContainText("idle at 30 fps");
    await expect.poll(() => shownFps(dash), { timeout: 10_000, message: "the FPS shows the idle rate" }).toBeLessThan(40);
    await page.locator(".v-card").filter({ has: notice }).first().screenshot({ path: test.info().outputPath("idle-notice.png") });

    await test.step("Full speed on the dashboard", async () => {
      await notice.getByRole("button", { name: "Full speed" }).click();
      await expect.poll(async () => (await robotState(request)).idleNow).toBe(false);
      expect((await robotState(request)).idleWhileDisabled).toBe(false);
      await expect(notice).toBeHidden({ timeout: 5_000 });
      await expect.poll(() => shownFps(dash), { timeout: 10_000, message: "full speed" }).toBeGreaterThan(100);
    });

    await test.step("back on from Settings > Robot state", async () => {
      await page.goto("/#/settings");
      const card = page.locator(".v-card").filter({ has: page.locator(".v-card-title", { hasText: /^Robot state$/ }) });
      await expect(card).toContainText("Robot disabled");
      await card.locator('[data-pv-control="switch"][data-pv-label="Idle while the robot is disabled"] input').check();
      await expect.poll(async () => (await robotState(request)).idleNow).toBe(true);
      await expect(card).toContainText("Cameras idling at 30 fps");
      await card.screenshot({ path: test.info().outputPath("robot-state-card.png") });
    });
  } finally {
    await request.post("/api/robotState", { data: { idleWhileDisabled: before.idleWhileDisabled, idleFps: before.idleFps } });
    await robot.stop();
  }
});

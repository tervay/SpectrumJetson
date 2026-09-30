import { test, expect } from "@playwright/test";
import { FakeRobot, JETSON, robotState } from "../lib/fake-robot";
import { cameraState, Dashboard, uiState } from "../lib/pv";
import { pickCamera, removeEverywhere, restoreRunning } from "../lib/fixtures";

// photonvision-51: "Event pipeline when the field connects". A profile (zz-uitest-all on every
// camera) is the event pipeline; the fake robot connects disabled, then the FMS attaches: every
// camera with that number switches to it, once. A switch on the dashboard afterwards sticks, and
// "Switch now" switches again.

const ALL = "zz-uitest-all";

test("event pipeline when the field connects; overrides stick; Switch now", async ({ page, request }) => {
  test.skip(!JETSON, "needs PV_UI_JETSON (tests/ui/run.sh sets it) to start the fake robot");
  const saved = await robotState(request);
  test.skip(saved.robotConnected, "a robot is already connected");
  const home = await pickCamera(request);
  const dash = new Dashboard(page);
  await dash.open();
  await removeEverywhere(dash, request, [ALL], home);

  const before = await uiState(request);
  const homeState = before.find((c) => c.nickname === home)!;
  // The profile, made through the API (the dialog has its own test).
  const created = await request.post("/api/settings/createPipeline", {
    data: {
      name: ALL,
      fromCamera: homeState.uniqueName,
      fromPipeline: homeState.currentPipelineIndex,
      cameras: before.map((c) => c.uniqueName)
    }
  });
  expect(created.ok()).toBeTruthy();
  const withProfile = await uiState(request);
  // The number most cameras got it as.
  const counts = new Map<number, number>();
  for (const c of withProfile) {
    const i = c.pipelineNicknames.indexOf(ALL);
    counts.set(i, (counts.get(i) ?? 0) + 1);
  }
  const event = [...counts.entries()].sort((a, b) => b[1] - a[1])[0][0];

  const startedAt = Date.now();
  let robot: FakeRobot | undefined;
  try {
    await test.step("turn it on in Settings > Robot state", async () => {
      await page.goto("/#/settings");
      const card = page.locator(".v-card").filter({ has: page.locator(".v-card-title", { hasText: /^Robot state$/ }) });
      await card.locator('[data-pv-control="switch"][data-pv-label="Event pipeline when the field connects"] input').check();
      await expect.poll(async () => (await robotState(request)).eventProfileOnFms).toBe(true);
      const select = card.locator('[data-pv-control="select"][data-pv-label="Event pipeline"]');
      await select.locator(".v-field").click();
      const option = page.locator(".v-overlay--active .v-list-item").filter({ hasText: new RegExp(`^\\s*${event}: `) });
      await expect(option).toContainText(ALL);
      await option.click();
      await expect.poll(async () => (await robotState(request)).eventPipeline).toBe(event);
      await card.screenshot({ path: test.info().outputPath("event-profile-card.png") });
    });

    await test.step("the field connects: every camera with that number switches", async () => {
      robot = await FakeRobot.start(request, "disabled:4", "fms-disabled:40");
      await expect.poll(async () => (await robotState(request)).fmsAttached, { timeout: 15_000 }).toBe(true);
      for (const c of withProfile) {
        const expected = c.pipelineNicknames.length > event ? event : c.currentPipelineIndex;
        await expect
          .poll(async () => (await cameraState(request, c.nickname)).currentPipelineIndex, { message: c.nickname })
          .toBe(expected);
      }
      const last = (await robotState(request)).lastEventSwitch;
      console.log(`Event profile: ${last}`);
      expect(last).toContain("the field (FMS) connected");
      // A camera whose pipeline at that number isn't the profile is named.
      for (const c of withProfile) {
        const theirs = c.pipelineNicknames[event];
        if (theirs !== undefined && theirs !== ALL) expect(last).toContain(`${c.nickname} (its ${event} is '${theirs}')`);
      }
    });

    await test.step("a switch on the dashboard afterwards sticks", async () => {
      const back = homeState.pipelineNicknames[homeState.currentPipelineIndex];
      await page.goto("/#/dashboard");
      await dash.selectCamera(home);
      await dash.selectPipeline(back);
      await expect.poll(async () => (await cameraState(request, home)).currentPipelineIndex).toBe(homeState.currentPipelineIndex);
      await page.waitForTimeout(5_000); // the FMS is still attached; nothing may switch it back
      expect((await cameraState(request, home)).currentPipelineIndex).toBe(homeState.currentPipelineIndex);
    });

    await test.step("Switch now switches again", async () => {
      await page.goto("/#/settings");
      const card = page.locator(".v-card").filter({ has: page.locator(".v-card-title", { hasText: /^Robot state$/ }) });
      await card.getByRole("button", { name: "Switch now" }).click();
      await expect.poll(async () => (await cameraState(request, home)).currentPipelineIndex).toBe(event);
      await expect(card.getByTestId("last-event-switch")).toContainText("Switch now");
    });
  } finally {
    await robot?.stop();
    // The fake field also triggers the day's "field connected" snapshot (photonvision-53): delete
    // it, so a real one is still taken when the real field connects.
    const snaps = (await (await request.get("/api/snapshots")).json()) as { id: string; reason: string; createdAt: number }[];
    for (const s of snaps) {
      if (s.reason === "field connected" && s.createdAt >= startedAt) await request.post("/api/snapshots/delete", { data: { id: s.id } });
    }
    await request.post("/api/robotState", {
      data: { eventProfileOnFms: saved.eventProfileOnFms, eventPipeline: saved.eventPipeline }
    });
    await page.goto("/#/dashboard");
    await removeEverywhere(dash, request, [ALL], home);
    await restoreRunning(dash, request, before);
  }
});

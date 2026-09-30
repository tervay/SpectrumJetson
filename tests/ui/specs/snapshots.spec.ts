import { test, expect } from "@playwright/test";
import { uiState } from "../lib/pv";
import { pickCamera } from "../lib/fixtures";

// photonvision-53: settings snapshots. Save a named snapshot in Settings, change something (a
// pipeline zz-uitest-snap on one camera), restore the snapshot: PhotonVision restarts with the
// pipeline gone, and the automatic "Before restoring" snapshot has it. The restore itself puts the
// Jetson back as it was; the test snapshots are deleted afterwards.

const NAME = "zz-uitest snapshot";
const EXTRA = "zz-uitest-snap";

interface Snapshot {
  id: string;
  name: string;
  reason: string;
  cameras: { nickname: string; pipelines: string[] }[];
}

test("save a snapshot, change a pipeline, restore it", async ({ page, request }) => {
  test.setTimeout(3 * 60_000); // includes a PhotonVision restart
  const camera = await pickCamera(request);
  const list = async () => (await (await request.get("/api/snapshots")).json()) as Snapshot[];
  const deleteSnap = (id: string) => request.post("/api/snapshots/delete", { data: { id } });
  for (const s of await list()) if (s.name === NAME || s.name.includes(`'${NAME}'`)) await deleteSnap(s.id);

  await page.goto("/#/settings");
  const card = page.locator(".v-card").filter({ hasText: "Settings snapshots" });
  await card.getByTestId("snapshot-name").locator("input").fill(NAME);
  await card.getByTestId("snapshot-save").click();
  await expect(card.getByTestId("snapshot-table")).toContainText(NAME);
  const saved = (await list()).find((s) => s.name === NAME)!;
  expect(saved.reason).toBe("manual");
  const before = await uiState(request);
  expect(saved.cameras.map((c) => c.nickname).sort()).toEqual(before.map((c) => c.nickname).sort());

  let restored = false;
  try {
    // The change: an extra pipeline on one camera (not switched to).
    const home = before.find((c) => c.nickname === camera)!;
    const made = await request.post("/api/settings/createPipeline", {
      data: { name: EXTRA, type: 5, cameras: [home.uniqueName] }
    });
    expect(made.ok()).toBeTruthy();
    await expect.poll(async () => (await uiState(request)).find((c) => c.nickname === camera)!.pipelineNicknames).toContain(EXTRA);

    await page.reload();
    await card.getByTestId(`snapshot-${saved.id}`).getByTestId("snapshot-restore").click();
    await page.getByTestId("snapshot-restore-confirm").click();
    restored = true;
    await expect(page.locator(".v-snackbar__content").last()).toContainText("PhotonVision is restarting");

    // Back after the restart, without the extra pipeline, every camera as before.
    await expect
      .poll(
        async () => {
          try {
            return (await uiState(request)).find((c) => c.nickname === camera)?.pipelineNicknames.includes(EXTRA);
          } catch {
            return undefined; // restarting
          }
        },
        { timeout: 90_000, intervals: [2000] }
      )
      .toBe(false);
    const after = await uiState(request);
    for (const c of before) {
      const now = after.find((a) => a.nickname === c.nickname)!;
      expect(now.pipelineNicknames, c.nickname).toEqual(c.pipelineNicknames);
      expect(now.currentPipelineIndex, c.nickname).toBe(c.currentPipelineIndex);
    }
    const auto = (await list()).find((s) => s.name === `Before restoring '${NAME}'`)!;
    expect(auto.reason).toBe("before restore");
    expect(auto.cameras.find((c) => c.nickname === camera)!.pipelines).toContain(EXTRA);

    // The dashboard reconnects and shows the snapshot list again.
    await page.goto("/#/settings");
    await expect(card.getByTestId("snapshot-table")).toContainText(`Before restoring '${NAME}'`, { timeout: 20_000 });
    await card.screenshot({ path: test.info().outputPath("snapshots-card.png") });
  } finally {
    if (!restored) {
      // The restore never ran: take the extra pipeline back out the same way.
      await request.post("/api/snapshots/restore", { data: { id: saved.id } }).catch(() => undefined);
      await expect.poll(async () => (await uiState(request).catch(() => [])).length, { timeout: 90_000 }).toBeGreaterThan(0);
    }
    for (const s of await list()) if (s.name === NAME || s.name.includes(`'${NAME}'`)) await deleteSnap(s.id);
  }
});

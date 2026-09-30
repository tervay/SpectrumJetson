import { test, expect, secondDashboard } from "../lib/fixtures";
import {
  backendValues,
  cameraState,
  changeControl,
  changedKeys,
  isEnabled,
  listControls,
  readControl,
  sameValue,
  type ControlKind
} from "../lib/pv";

// The bug class this catches: a control that changes the camera but not the page (the gamma slider,
// photonvision-39), or the page but not the backend, or one dashboard but not another
// (photonvision-41). For every control on every tab of the test pipeline:
//   1. change it the way a person would;
//   2. this page shows the new value;
//   3. the backend's settings changed (/api/spectrum/uiState);
//   4. a second dashboard shows the new value;
//   5. put it back, and all three agree it's back.

// Not changed by this test: names, and the camera's video mode (switching modes on a live
// 5-camera USB budget is its own test; PV_UI_TEST_VIDEO_MODES=1 includes them).
const SKIP_LABELS = new Set(["Camera", "Pipeline", "Type"]);
// Only this browser's view, not a setting: the backend and other dashboards rightly don't change.
const LOCAL_ONLY_LABELS = new Set(["Draw on the stream"]);
const VIDEO_MODE_LABELS = new Set(["Resolution", "Stream Resolution"]);
const SKIP_TABS = new Set(["TARGETS"]);

interface Row {
  tab: string;
  kind: ControlKind;
  label: string;
  from: string;
  to: string;
  backendKeys: string;
  result: string;
}

test("every control round-trips: page, backend and a second dashboard", async ({
  dash,
  request,
  browser,
  camera,
  pipeline
}) => {
  const rows: Row[] = [];
  const other = await secondDashboard(browser, dash.page);
  await other.selectCamera(camera);
  await expect.poll(() => other.currentPipelineName()).toBe("zz-uitest");

  const tabs = (await dash.tabNames()).filter((t) => !SKIP_TABS.has(t.toUpperCase()));
  for (const tab of tabs) {
    const card = await dash.openTab(tab);
    const otherCard = await other.openTab(tab);
    for (const { kind, label } of await listControls(card)) {
      if (kind === "input" || SKIP_LABELS.has(label) || LOCAL_ONLY_LABELS.has(label)) continue;
      if (VIDEO_MODE_LABELS.has(label) && process.env.PV_UI_TEST_VIDEO_MODES !== "1") continue;
      const control = dash.control(kind, label, card);
      if (!(await control.isVisible()) || !(await isEnabled(control, kind))) {
        rows.push({ tab, kind, label, from: "", to: "", backendKeys: "", result: "skipped (disabled)" });
        continue;
      }
      await test.step(`${tab} / ${label}`, async () => {
        const step = parseFloat((await control.getAttribute("data-pv-step")) ?? "1") || 1;
        const same = (a: string, b: string) => sameValue(kind, a, b, step);
        const before = backendValues(await cameraState(request, camera));
        const from = await readControl(control, kind);
        const to = await changeControl(dash.page, control, kind);
        const row: Row = { tab, kind, label, from, to, backendKeys: "", result: "PASS" };
        rows.push(row);
        if (same(from, to)) {
          row.result = "skipped (only one value)";
          return;
        }
        const failures: string[] = [];
        const check = async (what: string, fn: () => Promise<boolean>) => {
          try {
            await expect.poll(fn, { timeout: 4_000 }).toBe(true);
          } catch {
            failures.push(what);
          }
        };

        await check("page didn't show the new value", async () => same(await readControl(control, kind), to));
        let keys: string[] = [];
        await check("backend didn't change", async () => {
          keys = changedKeys(before, backendValues(await cameraState(request, camera)));
          return keys.length > 0;
        });
        row.backendKeys = keys.join(" ");
        const otherControl = other.control(kind, label, otherCard);
        await check("second dashboard didn't update", async () =>
          (await otherControl.isVisible()) ? same(await readControl(otherControl, kind), to) : false
        );

        // Put it back.
        await changeControl(dash.page, control, kind, from);
        await check("page didn't go back", async () => same(await readControl(control, kind), from));
        await check("backend didn't go back", async () => {
          const now = backendValues(await cameraState(request, camera));
          return keys.every((k) => now.get(k) === before.get(k));
        });
        await check("second dashboard didn't go back", async () =>
          (await otherControl.isVisible()) ? same(await readControl(otherControl, kind), from) : false
        );
        if (failures.length) row.result = `FAIL: ${failures.join("; ")}`;
        console.log(`${row.result.padEnd(26)} ${tab} / ${label}  ${from} -> ${to}  [${row.backendKeys}]`);
        expect.soft(failures, `${tab} / ${label}`).toEqual([]);
      });
    }
  }

  const table = rows
    .map((r) => `${r.result.padEnd(26)} ${`${r.tab} / ${r.label}`.padEnd(46)} ${r.from} -> ${r.to}  [${r.backendKeys}]`)
    .join("\n");
  console.log(`\n${camera}, pipeline ${pipeline.originalName} (copied to zz-uitest):\n${table}\n`);
  await test.info().attach("controls.txt", { body: table, contentType: "text/plain" });
  expect(rows.filter((r) => r.result === "PASS").length, "controls tested").toBeGreaterThan(5);
  await other.page.context().close();
});

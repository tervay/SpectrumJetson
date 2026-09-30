import { test, expect, removeEverywhere, restoreRunning } from "../lib/fixtures";
import { cameraState, changeControl, uiState, type CameraState, type Dashboard } from "../lib/pv";
import type { APIRequestContext } from "@playwright/test";

// photonvision-46: the new-pipeline dialog's "Start from" and "Create on every camera", and the
// pipeline menu's "Switch every camera to pipeline N".

const COPY = "zz-uitest-copy";
const ALL = "zz-uitest-all";

// Settings that are the pipeline's identity, not its settings.
const IDENTITY = new Set(["pipelineIndex", "pipelineNickname"]);

function settingsOf(camera: CameraState, name: string): Record<string, unknown> {
  const index = camera.pipelineNicknames.indexOf(name);
  expect(index, `${camera.nickname} has a pipeline called ${name}`).toBeGreaterThanOrEqual(0);
  const settings = camera.pipelines[index];
  // Guard against comparing two error entries and calling them equal.
  expect(settings?.pipelineNickname, `${camera.nickname}'s saved settings for ${name}`).toBe(name);
  return settings;
}

function differences(a: Record<string, unknown>, b: Record<string, unknown>, ignore: Set<string>): string[] {
  const keys = new Set([...Object.keys(a), ...Object.keys(b)]);
  return [...keys].filter((k) => !ignore.has(k) && JSON.stringify(a[k]) !== JSON.stringify(b[k]));
}

async function createPipeline(dash: Dashboard, name: string, startFrom: string, everyCamera: boolean) {
  await dash.pipelineMenu("mdi-plus");
  const dialog = dash.page.locator(".v-overlay--active .v-card").filter({ hasText: "Create New Pipeline" });
  await expect(dialog).toBeVisible();
  await dash.control("input", "Pipeline Name", dialog).locator("input").fill(name);
  await dash.chooseInSelect(dash.control("select", "Start from", dialog), startFrom);
  if (everyCamera) await dialog.getByLabel(/Create on every camera/).check();
  await dialog.getByRole("button", { name: "Create" }).click();
  await expect(dialog).toBeHidden();
}

test("Start from a copy of this camera's pipeline: an exact copy", async ({ dash, request, camera, pipeline }) => {
  await removeEverywhere(dash, request, [COPY], camera);
  await dash.selectPipeline("zz-uitest");
  // Make the source distinctive.
  const card = await dash.openTab("APRILCUDATAG");
  await changeControl(dash.page, dash.control("slider", "Decision Margin Cutoff", card), "slider", "23");
  await expect.poll(async () => (await cameraState(request, camera)).currentPipelineSettings.decisionMargin).toBe(23);
  const source = settingsOf(await cameraState(request, camera), "zz-uitest");
  const sourceIndex = (await cameraState(request, camera)).pipelineNicknames.indexOf("zz-uitest");

  try {
    await createPipeline(dash, COPY, `Copy of ${camera} / ${sourceIndex}: zz-uitest`, false);
    expect(await dash.snackbar()).toContain(`Created '${COPY}'`);
    // This camera switches to it; it has every setting of the source.
    await expect.poll(async () => (await cameraState(request, camera)).currentPipelineSettings.pipelineNickname).toBe(COPY);
    const state = await cameraState(request, camera);
    expect(differences(source, settingsOf(state, COPY), IDENTITY)).toEqual([]);
    await expect.poll(() => dash.currentPipelineName()).toBe(COPY);
  } finally {
    await removeEverywhere(dash, request, [COPY], camera);
    await dash.selectPipeline("zz-uitest");
  }
  void pipeline;
});

test("Create on every camera from another camera's pipeline, then Switch all", async ({
  dash,
  request,
  camera,
  pipeline
}) => {
  await removeEverywhere(dash, request, [ALL], camera);
  await dash.selectPipeline("zz-uitest");
  // A distinctive source: decision margin 23, and an orientation the other cameras mustn't take.
  let card = await dash.openTab("APRILCUDATAG");
  await changeControl(dash.page, dash.control("slider", "Decision Margin Cutoff", card), "slider", "23");
  // The slider sends 20 ms after the last change; switching tabs before then drops it.
  await expect.poll(async () => (await cameraState(request, camera)).currentPipelineSettings.decisionMargin).toBe(23);
  card = await dash.openTab("INPUT");
  await changeControl(dash.page, dash.control("select", "Orientation", card), "select", "90° CW");
  await expect.poll(async () => (await cameraState(request, camera)).currentPipelineSettings.inputImageRotationMode).not.toBe(
    settingsOf(await cameraState(request, camera), pipeline.originalName).inputImageRotationMode
  );

  const before = await uiState(request);
  const running = new Map(before.map((c) => [c.nickname, c.currentPipelineIndex]));
  const source = settingsOf(before.find((c) => c.nickname === camera)!, "zz-uitest");
  const sourceIndex = before.find((c) => c.nickname === camera)!.pipelineNicknames.indexOf("zz-uitest");

  try {
    await test.step("create on every camera", async () => {
      await createPipeline(dash, ALL, `Copy of ${camera} / ${sourceIndex}: zz-uitest`, true);
      const message = await dash.snackbar();
      console.log(`Create on every camera: ${message}`);
      expect(message).toContain(`Created '${ALL}'`);
      await expect
        .poll(async () => (await uiState(request)).every((c) => c.pipelineNicknames.includes(ALL)))
        .toBe(true);
      const after = await uiState(request);
      for (const c of after) {
        const copy = settingsOf(c, ALL);
        // The snackbar names the number each camera got it as.
        expect(message).toContain(c.nickname);
        if (c.nickname === camera) {
          expect(c.currentPipelineSettings.pipelineNickname, "this camera switches to it").toBe(ALL);
          expect(differences(source, copy, IDENTITY), `${c.nickname}: an exact copy`).toEqual([]);
        } else {
          expect(c.currentPipelineIndex, `${c.nickname} stays on its pipeline`).toBe(running.get(c.nickname));
          expect(copy.decisionMargin, `${c.nickname} got the copied settings`).toBe(23);
          // Orientation belongs to the camera: the others keep theirs, not the source's 90° CW.
          expect(copy.inputImageRotationMode, `${c.nickname} keeps its own orientation`).not.toEqual(
            source.inputImageRotationMode
          );
        }
      }
    });

    await test.step("switch every camera to this pipeline's number", async () => {
      const state = await uiState(request);
      const index = state.find((c) => c.nickname === camera)!.currentPipelineIndex;
      await dash.pipelineMenu("mdi-swap-horizontal");
      const message = await dash.snackbar();
      console.log(`Switch all: ${message}`);
      for (const c of state) {
        if (c.nickname === camera) continue;
        if (c.pipelineNicknames.length > index) {
          await expect.poll(async () => (await cameraState(request, c.nickname)).currentPipelineIndex).toBe(index);
          if (c.pipelineNicknames[index] !== ALL) expect(message).toContain(`its ${index} is '${c.pipelineNicknames[index]}'`);
        } else {
          expect(message).toContain(`${c.nickname} (no pipeline ${index})`);
          expect((await cameraState(request, c.nickname)).currentPipelineIndex, `${c.nickname} stays`).toBe(c.currentPipelineIndex);
        }
      }
    });
  } finally {
    await removeEverywhere(dash, request, [ALL], camera);
    await restoreRunning(dash, request, before, camera);
    await dash.selectCamera(camera);
    await dash.selectPipeline("zz-uitest");
  }
});

test("the Pipeline dropdown numbers pipelines as PhotonVision does", async ({ dash, request, camera, pipeline }) => {
  const state = await cameraState(request, camera);
  const select = dash.control("select", "Pipeline");
  await select.locator(".v-field").click();
  const options = (await dash.page.locator(".v-overlay--active .v-list-item").allInnerTexts()).map((t) => t.trim());
  await dash.page.keyboard.press("Escape");
  expect(options).toEqual(state.pipelineNicknames.map((name, index) => `${index}: ${name}`));
  expect(await dash.currentPipeline()).toEqual({ index: state.currentPipelineIndex, name: "zz-uitest" });
  void pipeline;
});

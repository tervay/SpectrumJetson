import { test as base, expect, type APIRequestContext, type Browser, type Page } from "@playwright/test";
import { cameraState, Dashboard, TEST_PIPELINE, uiState, type CameraState } from "./pv";

// Every test runs on a temporary copy of the camera's current pipeline, called zz-uitest, and
// deletes it afterwards, so the real pipelines are never touched. A run that died half way
// leaves a zz-uitest behind; the next run deletes it first.

export async function pickCamera(request: APIRequestContext): Promise<string> {
  const cameras = await uiState(request);
  const wanted = process.env.PV_UI_CAMERA;
  if (wanted) {
    if (!cameras.some((c) => c.nickname === wanted)) throw new Error(`PV_UI_CAMERA=${wanted}: no such camera`);
    return wanted;
  }
  return (cameras.find((c) => c.nickname === "TopLeft") ?? cameras[0]).nickname;
}

export async function deletePipeline(dash: Dashboard, request: APIRequestContext, camera: string, name: string) {
  await dash.selectCamera(camera);
  await dash.selectPipeline(name);
  await expect.poll(async () => (await cameraState(request, camera)).currentPipelineSettings.pipelineNickname).toBe(name);
  await dash.pipelineMenu("mdi-trash-can-outline");
  await dash.page.locator(".v-overlay--active .v-btn").filter({ hasText: "Delete Pipeline" }).click();
  await expect
    .poll(async () => (await cameraState(request, camera)).pipelineNicknames, { timeout: 10_000 })
    .not.toContain(name);
}

export interface TestPipeline {
  camera: string;
  originalName: string;
  originalIndex: number;
}

export async function createTestPipeline(
  dash: Dashboard,
  request: APIRequestContext,
  camera: string
): Promise<TestPipeline> {
  await dash.selectCamera(camera);
  let state = await cameraState(request, camera);
  if (state.pipelineNicknames.includes(TEST_PIPELINE)) {
    const leftover = state.pipelineNicknames.indexOf(TEST_PIPELINE);
    // Where to go back to afterwards: the pipeline before the leftover, or the first one.
    await deletePipeline(dash, request, camera, TEST_PIPELINE);
    state = await cameraState(request, camera);
    await dash.selectPipeline(state.pipelineNicknames[Math.max(0, leftover - 1)] ?? state.pipelineNicknames[0]);
    state = await cameraState(request, camera);
  }
  const originalName = state.pipelineNicknames[state.currentPipelineIndex];
  const originalIndex = state.currentPipelineIndex;
  const before = state.pipelineNicknames;

  await dash.selectPipeline(originalName);
  await dash.pipelineMenu("mdi-content-copy");
  await expect.poll(async () => (await cameraState(request, camera)).pipelineNicknames.length).toBe(before.length + 1);
  state = await cameraState(request, camera);
  const copy = state.pipelineNicknames.find((n) => !before.includes(n))!;
  await dash.selectPipeline(copy);
  await expect.poll(async () => (await cameraState(request, camera)).currentPipelineSettings.pipelineNickname).toBe(copy);

  await dash.pipelineMenu("mdi-pencil");
  const nameField = dash.control("input", "Pipeline").locator("input");
  await nameField.fill(TEST_PIPELINE);
  await nameField.press("Enter");
  await expect
    .poll(async () => (await cameraState(request, camera)).currentPipelineSettings.pipelineNickname)
    .toBe(TEST_PIPELINE);
  await expect.poll(() => dash.currentPipelineName()).toBe(TEST_PIPELINE);
  return { camera, originalName, originalIndex };
}

export async function removeTestPipeline(dash: Dashboard, request: APIRequestContext, pipeline: TestPipeline) {
  await deletePipeline(dash, request, pipeline.camera, TEST_PIPELINE);
  await dash.selectPipeline(pipeline.originalName);
  await expect
    .poll(async () => (await cameraState(request, pipeline.camera)).currentPipelineIndex)
    .toBe(pipeline.originalIndex);
}

/** A second dashboard in its own browser context: a different laptop, as far as PhotonVision knows. */
export async function secondDashboard(browser: Browser, page: Page): Promise<Dashboard> {
  const context = await browser.newContext({
    baseURL: test.info().project.use.baseURL,
    viewport: page.viewportSize() ?? undefined
  });
  const dash = new Dashboard(await context.newPage());
  await dash.open();
  return dash;
}

/** Delete test pipelines on every camera and put each camera back on the pipeline it was running. */
export async function removeEverywhere(dash: Dashboard, request: APIRequestContext, names: string[], home: string) {
  for (const camera of await uiState(request)) {
    const running = camera.pipelineNicknames[camera.currentPipelineIndex];
    const leftovers = names.filter((n) => camera.pipelineNicknames.includes(n));
    if (!leftovers.length) continue;
    for (const name of leftovers) await deletePipeline(dash, request, camera.nickname, name);
    const back = names.includes(running) ? undefined : running;
    if (back) {
      await dash.selectPipeline(back);
      await expect.poll(async () => (await cameraState(request, camera.nickname)).currentPipelineSettings.pipelineNickname).toBe(back);
    }
  }
  await dash.selectCamera(home);
}

/** Put every camera except `skip` back on the pipeline (by name) it was running in `before`. */
export async function restoreRunning(dash: Dashboard, request: APIRequestContext, before: CameraState[], skip?: string) {
  for (const c of await uiState(request)) {
    if (c.nickname === skip) continue;
    const wanted = before.find((b) => b.nickname === c.nickname);
    if (!wanted) continue;
    const name = wanted.pipelineNicknames[wanted.currentPipelineIndex];
    if (c.pipelineNicknames[c.currentPipelineIndex] !== name) {
      await dash.selectCamera(c.nickname);
      await dash.selectPipeline(name);
    }
    await expect
      .poll(async () => (await cameraState(request, c.nickname)).currentPipelineIndex)
      .toBe(wanted.currentPipelineIndex);
  }
}

export const test = base.extend<{ dash: Dashboard; camera: string; pipeline: TestPipeline }>({
  dash: async ({ page }, use) => {
    const dash = new Dashboard(page);
    await dash.open();
    await use(dash);
  },
  camera: async ({ request }, use) => {
    await use(await pickCamera(request));
  },
  pipeline: async ({ dash, request, camera }, use) => {
    const pipeline = await createTestPipeline(dash, request, camera);
    try {
      await use(pipeline);
    } finally {
      // Back to the dashboard in a known state before cleaning up.
      await dash.page.keyboard.press("Escape");
      await removeTestPipeline(dash, request, pipeline);
    }
  }
});

export { expect };

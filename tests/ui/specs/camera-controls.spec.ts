import { test, expect } from "../lib/fixtures";
import { cameraState } from "../lib/pv";

// photonvision-54: the Input tab says how this camera's tags look (TagContrast), and a camera
// without a gain control (the Thriftiest Cam, though its quirks list gain) has no Camera Gain slider.

test("Input tab: tag contrast readout, no Camera Gain slider without gain", async ({ dash, request, camera }) => {
  await dash.selectCamera(camera);
  const card = await dash.openTab("INPUT");
  const readout = card.getByTestId("tag-contrast");
  await expect(readout).toBeVisible();
  const api = (await (await request.get("/api/tagContrast")).json())[camera];
  expect(api, `/api/tagContrast has ${camera}`).toBeTruthy();
  if (api.verdict === "no tags") await expect(readout).toContainText("no tag close enough to measure");
  else await expect(readout).toContainText(`white ${api.white}, black ${api.black}`);

  // The camera itself says whether it has gain: cscore lists "gain" among its properties or not.
  const state = await cameraState(request, camera);
  const hasGain = (state.currentPipelineSettings.cameraGain as number) >= 0;
  await expect(dash.control("slider", "Camera Gain", card)).toHaveCount(hasGain ? 1 : 0);
  console.log(`${camera}: tag contrast ${api.verdict}; gain slider ${hasGain ? "shown" : "hidden"}`);
});

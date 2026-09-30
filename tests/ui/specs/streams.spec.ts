import { test as base, expect } from "@playwright/test";
import { cameraState, Dashboard } from "../lib/pv";
import { pickCamera } from "../lib/fixtures";
import type { Page } from "@playwright/test";

// photonvision-48: a hidden dashboard tab closes its streams, so the Jetson stops encoding them
// (photonvision-15 encodes only while a stream has a viewer), and reopens them when shown again.

const test = base;

async function setHidden(page: Page, hidden: boolean) {
  // What the browser does when another tab comes to the front or the window is minimised.
  await page.evaluate((h) => {
    Object.defineProperty(document, "visibilityState", { configurable: true, get: () => (h ? "hidden" : "visible") });
    Object.defineProperty(document, "hidden", { configurable: true, get: () => h });
    document.dispatchEvent(new Event("visibilitychange"));
  }, hidden);
}

test("a hidden tab closes its streams and reopens them when shown", async ({ page, request }) => {
  const camera = await pickCamera(request);
  const watched = async () => {
    const v = (await cameraState(request, camera)).streamViewers;
    return v.input || v.output;
  };
  test.skip(await watched(), `another dashboard is already watching ${camera}; close it to run this test`);

  const dash = new Dashboard(page);
  await dash.open();
  await dash.selectCamera(camera);
  await expect.poll(watched, { timeout: 10_000, message: "the dashboard's stream reached the Jetson" }).toBe(true);

  await setHidden(page, true);
  await expect.poll(watched, { timeout: 10_000, message: "hidden: the Jetson has no viewer" }).toBe(false);

  await setHidden(page, false);
  await expect.poll(watched, { timeout: 10_000, message: "shown again: the stream is back" }).toBe(true);
  await expect
    .poll(() => page.locator("img[id$='camera-stream']:visible").first().evaluate((img: HTMLImageElement) => img.naturalWidth))
    .toBeGreaterThan(0);
});

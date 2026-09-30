import { test, expect, secondDashboard } from "../lib/fixtures";
import { cameraState, changeControl, type Dashboard } from "../lib/pv";
import type { APIRequestContext, Locator } from "@playwright/test";

// The detection mask editor (photonvision-40): draw, resize from a corner, move, and delete a box
// on the stream, and check the backend and a second dashboard follow each step.

interface Box {
  x: number;
  y: number;
  w: number;
  h: number;
}

async function backendMask(request: APIRequestContext, camera: string): Promise<{ mode: number; boxes: Box[] }> {
  return (await cameraState(request, camera)).currentPipelineSettings.detectionMask as { mode: number; boxes: Box[] };
}

const near = (a: number, b: number) => Math.abs(a - b) < 0.02;
const boxIs = (b: Box | undefined, want: Box) =>
  !!b && near(b.x, want.x) && near(b.y, want.y) && near(b.w, want.w) && near(b.h, want.h);

/** The editable overlay on whichever stream is showing (raw or processed), once its image has loaded. */
async function drawingSurface(dash: Dashboard): Promise<{ svg: Locator; at: (x: number, y: number) => [number, number] }> {
  const overlay = dash.page.locator(".pv-mask-overlay").filter({ has: dash.page.locator("svg.pv-mask-svg.editing") }).first();
  await expect
    .poll(() => overlay.locator("img").evaluate((img: HTMLImageElement) => img.naturalWidth), { timeout: 15_000 })
    .toBeGreaterThan(0);
  const svg = overlay.locator("svg.pv-mask-svg.editing");
  await expect(svg).toBeVisible();
  const r = (await svg.boundingBox())!;
  return { svg, at: (x, y) => [r.x + x * r.width, r.y + y * r.height] };
}

test("mask editor: draw, resize, move and delete a box", async ({ dash, request, browser, camera, pipeline }) => {
  const card = await dash.openTab("MASK");
  await changeControl(dash.page, dash.control("select", "Mask", card), "select", "Ignore inside the boxes");
  await expect.poll(async () => (await backendMask(request, camera)).mode).toBe(1);
  await changeControl(dash.page, dash.control("switch", "Draw on the stream", card), "switch", "true");

  const other = await secondDashboard(browser, dash.page);
  await other.selectCamera(camera);
  await expect.poll(() => other.currentPipelineName()).toBe("zz-uitest");
  const otherCard = await other.openTab("MASK");
  const otherRows = otherCard.locator("tbody tr");

  const { at } = await drawingSurface(dash);
  const mouse = dash.page.mouse;
  const drag = async (from: [number, number], to: [number, number]) => {
    await mouse.move(...from);
    await mouse.down();
    await mouse.move(...to, { steps: 8 });
    await mouse.up();
  };

  await test.step("draw", async () => {
    await drag(at(0.2, 0.2), at(0.5, 0.6));
    await expect.poll(async () => boxIs((await backendMask(request, camera)).boxes[0], { x: 0.2, y: 0.2, w: 0.3, h: 0.4 })).toBe(true);
    await expect(card.locator("tbody tr")).toHaveCount(1);
    await expect(otherRows).toHaveCount(1);
  });

  await test.step("resize from the bottom-right corner (top-left stays put)", async () => {
    await drag(at(0.5, 0.6), at(0.7, 0.8));
    await expect.poll(async () => boxIs((await backendMask(request, camera)).boxes[0], { x: 0.2, y: 0.2, w: 0.5, h: 0.6 })).toBe(true);
  });

  await test.step("resize from the top-left corner (bottom-right stays put)", async () => {
    await drag(at(0.2, 0.2), at(0.3, 0.3));
    await expect.poll(async () => boxIs((await backendMask(request, camera)).boxes[0], { x: 0.3, y: 0.3, w: 0.4, h: 0.5 })).toBe(true);
  });

  await test.step("move", async () => {
    await drag(at(0.5, 0.5), at(0.4, 0.45));
    await expect.poll(async () => boxIs((await backendMask(request, camera)).boxes[0], { x: 0.2, y: 0.25, w: 0.4, h: 0.5 })).toBe(true);
    await expect(otherCard.locator("tbody tr td").nth(1)).toHaveText(/20/);
  });

  await test.step("a second box, then Delete removes only the selected one", async () => {
    await drag(at(0.75, 0.1), at(0.9, 0.2));
    await expect.poll(async () => (await backendMask(request, camera)).boxes.length).toBe(2);
    await expect(otherRows).toHaveCount(2);
    await mouse.click(...at(0.4, 0.5)); // select the first box
    await dash.page.keyboard.press("Delete");
    await expect.poll(async () => (await backendMask(request, camera)).boxes.length).toBe(1);
    expect(boxIs((await backendMask(request, camera)).boxes[0], { x: 0.75, y: 0.1, w: 0.15, h: 0.1 })).toBe(true);
    await expect(otherRows).toHaveCount(1);
  });

  await test.step("Remove all boxes, mask off", async () => {
    await card.getByRole("button", { name: "Remove all boxes" }).click();
    await expect.poll(async () => (await backendMask(request, camera)).boxes.length).toBe(0);
    await changeControl(dash.page, dash.control("select", "Mask", card), "select", "Off");
    await expect.poll(async () => (await backendMask(request, camera)).mode).toBe(0);
    await expect(otherRows).toHaveCount(0);
  });

  await changeControl(dash.page, dash.control("switch", "Draw on the stream", card), "switch", "false");
  await other.page.context().close();
  void pipeline; // the fixture made the zz-uitest copy this test works on
});

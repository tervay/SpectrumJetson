import { expect, type APIRequestContext, type Locator, type Page } from "@playwright/test";

// Helpers for driving the PhotonVision dashboard. Controls are found by the data-pv-control /
// data-pv-label hooks from photonvision-45, never by Vuetify's generated ids.

export type ControlKind = "slider" | "switch" | "select" | "range" | "number" | "radio" | "input";

export interface CameraState {
  uniqueName: string;
  nickname: string;
  currentPipelineIndex: number;
  pipelineNicknames: string[];
  currentPipelineSettings: Record<string, unknown>;
  extraControls: { key: string; label: string; value: number; default: number }[];
  /** Whether each stream has a viewer (photonvision-48). */
  streamViewers: { input: boolean; output: boolean };
  /** Every pipeline's saved settings, by index (photonvision-46). */
  pipelines: Record<string, unknown>[];
}

export const TEST_PIPELINE = "zz-uitest";

export function stableLabel(label: string): string {
  return label.replace(/\s*\(.*\)\s*$/, "");
}

/** The backend's own view (photonvision-45's /api/spectrum/uiState). */
export async function uiState(request: APIRequestContext): Promise<CameraState[]> {
  // A few tries: a read can land in the middle of a pipeline being deleted.
  for (let attempt = 1; ; attempt++) {
    const response = await request.get("/api/spectrum/uiState");
    if (response.ok()) return (await response.json()).cameras;
    if (attempt === 3) throw new Error(`/api/spectrum/uiState answered ${response.status()}: ${await response.text()}`);
    await new Promise((r) => setTimeout(r, 200));
  }
}

export async function cameraState(request: APIRequestContext, nickname: string): Promise<CameraState> {
  const camera = (await uiState(request)).find((c) => c.nickname === nickname);
  if (!camera) throw new Error(`No camera called ${nickname} on the Jetson`);
  return camera;
}

/** Everything the backend holds for a camera's current pipeline, flattened to key -> JSON. */
export function backendValues(camera: CameraState): Map<string, string> {
  const values = new Map<string, string>();
  // photonvision-28's extra controls store -1 for "the camera's default": the same as the default.
  const defaults = new Map(camera.extraControls.map((c) => [c.key, c.default]));
  for (const [key, value] of Object.entries(camera.currentPipelineSettings)) {
    values.set(key, JSON.stringify(value === -1 && defaults.has(key) ? defaults.get(key) : value));
  }
  for (const control of camera.extraControls) values.set(`extra.${control.key}`, JSON.stringify(control.value));
  return values;
}

export function changedKeys(before: Map<string, string>, after: Map<string, string>): string[] {
  return [...after.keys()].filter((key) => before.get(key) !== after.get(key));
}

export class Dashboard {
  constructor(readonly page: Page) {}

  async open(): Promise<void> {
    await this.page.goto("/#/dashboard");
    await expect(this.page.getByText("Backend connected")).toBeVisible({ timeout: 20_000 });
    await expect(this.control("select", "Camera").locator(".v-select__selection-text")).not.toBeEmpty();
  }

  /** Labels like "Exposure (5.0 ms)" carry their value, so match on the part before the brackets. */
  control(kind: ControlKind, label: string, scope: Locator | Page = this.page): Locator {
    const stem = stableLabel(label);
    const exact = `[data-pv-control="${kind}"][data-pv-label="${stem}"]`;
    const withValue = `[data-pv-control="${kind}"][data-pv-label^="${stem} ("]`;
    return scope.locator(`${exact}, ${withValue}`).first();
  }

  async chooseInSelect(select: Locator, optionText: string): Promise<void> {
    await select.locator(".v-field").click();
    const option = this.page
      .locator(".v-overlay--active .v-list-item")
      .filter({ has: this.page.getByText(optionText, { exact: true }) });
    await option.first().click();
    await expect(this.page.locator(".v-overlay--active .v-list")).toHaveCount(0);
  }

  async selectCamera(nickname: string): Promise<void> {
    const select = this.control("select", "Camera");
    if ((await select.locator(".v-select__selection-text").innerText()).trim() === nickname) return;
    await this.chooseInSelect(select, nickname);
    await expect(select.locator(".v-select__selection-text")).toHaveText(nickname);
  }

  /** The Pipeline dropdown shows "N: name" (photonvision-47). */
  async currentPipeline(): Promise<{ index: number; name: string }> {
    const text = (await this.control("select", "Pipeline").locator(".v-select__selection-text").innerText()).trim();
    const m = /^(\d+): (.*)$/.exec(text);
    return m ? { index: parseInt(m[1]), name: m[2] } : { index: -1, name: text };
  }

  async currentPipelineName(): Promise<string> {
    return (await this.currentPipeline()).name;
  }

  async selectPipeline(name: string): Promise<void> {
    if ((await this.currentPipelineName()) === name) return;
    const select = this.control("select", "Pipeline");
    await select.locator(".v-field").click();
    await this.page
      .locator(".v-overlay--active .v-list-item")
      .filter({ hasText: new RegExp(`^\\s*\\d+: ${name.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}\\s*$`) })
      .first()
      .click();
    await expect(this.page.locator(".v-overlay--active .v-list")).toHaveCount(0);
    await expect.poll(() => this.currentPipelineName()).toBe(name);
  }

  /** Click one of the pipeline menu (☰) entries, found by its icon. */
  async pipelineMenu(icon: string): Promise<void> {
    await this.control("select", "Pipeline").locator("xpath=../..").locator(".v-icon.mdi-menu").click();
    await this.page.locator(`.v-overlay--active .v-list-item:has(.${icon})`).click();
  }

  /** The card holding a tab, and the tab clicked open. Returns the tab's card. */
  async openTab(name: string): Promise<Locator> {
    const tab = this.page.locator(".v-tab").filter({ hasText: new RegExp(`^\\s*${name}\\s*$`, "i") });
    await tab.click();
    await expect(tab).toHaveClass(/v-tab--selected/);
    return this.page.locator(".v-card").filter({ has: tab }).last();
  }

  /** The snackbar's text, once it shows. */
  async snackbar(): Promise<string> {
    const bar = this.page.locator(".v-snackbar__content").last();
    await expect(bar).toBeVisible();
    return (await bar.innerText()).trim();
  }

  async tabNames(): Promise<string[]> {
    return (await this.page.locator(".v-tab").allInnerTexts()).map((t) => t.trim());
  }
}

// ----- reading and changing one control, whatever its kind -----

export interface ControlInfo {
  kind: ControlKind;
  label: string;
}

export async function listControls(card: Locator): Promise<ControlInfo[]> {
  const found: ControlInfo[] = [];
  for (const el of await card.locator("[data-pv-control]:visible").all()) {
    const kind = (await el.getAttribute("data-pv-control")) as ControlKind;
    const label = stableLabel((await el.getAttribute("data-pv-label")) ?? "");
    if (label && !found.some((c) => c.kind === kind && c.label === label)) found.push({ kind, label });
  }
  return found;
}

/** A control's value as the page shows it, as a string (range: "lo,hi"). */
export async function readControl(control: Locator, kind: ControlKind): Promise<string> {
  switch (kind) {
    case "slider":
      return (await control.locator("input[type=number]").inputValue()).trim();
    case "range": {
      const fields = control.locator("input[type=number]");
      return `${(await fields.nth(0).inputValue()).trim()},${(await fields.nth(1).inputValue()).trim()}`;
    }
    case "number":
    case "input":
      return (await control.locator("input").first().inputValue()).trim();
    case "switch":
      return String(await control.locator("input[type=checkbox]").isChecked());
    case "select":
      return (await control.locator(".v-select__selection-text").allInnerTexts()).join("|").trim();
    case "radio": {
      const radios = control.locator("input[type=radio]");
      const n = await radios.count();
      for (let i = 0; i < n; i++) if (await radios.nth(i).isChecked()) return String(i);
      return "";
    }
  }
}

export async function isEnabled(control: Locator, kind: ControlKind): Promise<boolean> {
  const input =
    kind === "select" ? control.locator(".v-field") : control.locator("input").first();
  if (kind === "select") return !(await input.getAttribute("class"))?.includes("v-field--disabled");
  return await input.isEnabled();
}

function numberAttr(control: Locator, name: string) {
  return control.getAttribute(name).then((v) => (v === null ? NaN : parseFloat(v)));
}

/** Pick a value one step away that's still in range. */
function nextValue(current: number, min: number, max: number, step: number): number {
  const up = current + step;
  const value = up <= max ? up : current - step;
  return Math.max(min, Math.min(max, parseFloat(value.toFixed(6))));
}

export function sameValue(kind: ControlKind, a: string, b: string, step = 1): boolean {
  if (kind === "slider" || kind === "number") return Math.abs(parseFloat(a) - parseFloat(b)) < step / 2 + 1e-9;
  if (kind === "range") {
    const [a0, a1] = a.split(",").map(parseFloat);
    const [b0, b1] = b.split(",").map(parseFloat);
    return Math.abs(a0 - b0) < step / 2 + 1e-9 && Math.abs(a1 - b1) < step / 2 + 1e-9;
  }
  return a === b;
}

/**
 * Change a control to a different value, the way a person would: the arrow button for sliders,
 * a click for switches and radios, the menu for selects. Returns the value it should now show.
 * `target` sets a specific value instead (used to put things back).
 */
export async function changeControl(
  page: Page,
  control: Locator,
  kind: ControlKind,
  target?: string
): Promise<string> {
  switch (kind) {
    case "slider": {
      const field = control.locator("input[type=number]");
      if (target !== undefined) {
        await field.fill(target);
        await field.press("Enter");
        return target;
      }
      const current = parseFloat(await field.inputValue());
      const min = await numberAttr(control, "data-pv-min");
      const max = await numberAttr(control, "data-pv-max");
      const step = (await numberAttr(control, "data-pv-step")) || 1;
      const next = nextValue(current, min, max, step);
      // The arrow buttons are the path the gamma bug hid in (photonvision-39).
      await control.locator(next > current ? ".mdi-menu-right" : ".mdi-menu-left").click();
      return String(next);
    }
    case "range": {
      const fields = control.locator("input[type=number]");
      if (target !== undefined) {
        const [lo, hi] = target.split(",");
        await fields.nth(0).fill(lo);
        await fields.nth(1).fill(hi);
        await fields.nth(1).press("Tab");
        return target;
      }
      const lo = parseFloat(await fields.nth(0).inputValue());
      const hi = parseFloat(await fields.nth(1).inputValue());
      const min = await numberAttr(control, "data-pv-min");
      const step = (await numberAttr(control, "data-pv-step")) || 1;
      // Move the low end: up if there's room below the high end, otherwise down.
      const newLo = lo + step <= hi ? lo + step : Math.max(min, lo - step);
      await fields.nth(0).fill(String(newLo));
      await fields.nth(0).press("Tab");
      return `${newLo},${hi}`;
    }
    case "number": {
      const field = control.locator("input").first();
      const value = target ?? String(parseFloat(await field.inputValue()) + 1);
      await field.fill(value);
      await field.press("Tab");
      return value;
    }
    case "switch": {
      const box = control.locator("input[type=checkbox]");
      const want = target ?? String(!(await box.isChecked()));
      if (String(await box.isChecked()) !== want) await box.click({ force: true });
      return want;
    }
    case "radio": {
      const radios = control.locator("input[type=radio]");
      let index = target === undefined ? -1 : parseInt(target);
      if (index < 0) {
        const current = parseInt(await readControl(control, kind));
        const n = await radios.count();
        for (let i = 0; i < n && index < 0; i++) if (i !== current && (await radios.nth(i).isEnabled())) index = i;
      }
      await radios.nth(index).click({ force: true });
      return String(index);
    }
    case "select": {
      const current = await readControl(control, kind);
      await control.locator(".v-field").click();
      const options = page.locator(".v-overlay--active .v-list-item:not(.v-list-item--disabled)");
      await expect(options.first()).toBeVisible();
      const texts = (await options.allInnerTexts()).map((t) => t.trim());
      const want = target ?? texts.find((t) => t !== current);
      if (want === undefined) {
        await page.keyboard.press("Escape");
        return current;
      }
      await options.nth(texts.indexOf(want)).click();
      return want;
    }
    case "input":
      throw new Error("Text inputs aren't changed by the generic test");
  }
}

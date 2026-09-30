import { readFileSync } from "fs";

// Fails the run if any camera ends on a different pipeline than it started on, or a test pipeline
// (zz-uitest...) was left behind. A test that dies half way can leave either; the fix is on the
// dashboard (switch the camera back / delete the zz-uitest pipeline), and the list says where.
export default async function globalTeardown() {
  const base = process.env.PV_URL ?? "http://localhost:5800";
  let start: { camera: string; pipeline: string }[];
  try {
    start = JSON.parse(readFileSync(".state/start-pipelines.json", "utf8"));
  } catch {
    return; // setup never got that far
  }
  const cameras = (await (await fetch(`${base}/api/spectrum/uiState`)).json()).cameras as {
    nickname: string;
    currentPipelineIndex: number;
    pipelineNicknames: string[];
  }[];
  const problems: string[] = [];
  for (const c of cameras) {
    const now = c.pipelineNicknames[c.currentPipelineIndex] ?? String(c.currentPipelineIndex);
    const was = start.find((s) => s.camera === c.nickname)?.pipeline;
    if (was !== undefined && was !== now) problems.push(`${c.nickname} started on '${was}' and is now on '${now}'`);
    const leftovers = c.pipelineNicknames.filter((n) => n.startsWith("zz-uitest"));
    if (leftovers.length) problems.push(`${c.nickname} still has ${leftovers.join(", ")}`);
  }
  // Test snapshots (photonvision-53), and a "field connected" one taken during the run by the fake field.
  const snaps = (await (await fetch(`${base}/api/snapshots`)).json()) as { name: string; reason: string }[];
  for (const s of snaps) if (s.name.includes("zz-uitest")) problems.push(`snapshot '${s.name}' left behind`);
  if (problems.length) {
    throw new Error(`The tests left the Jetson changed:\n  ${problems.join("\n  ")}`);
  }
}

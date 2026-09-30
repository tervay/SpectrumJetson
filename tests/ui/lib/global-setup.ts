import { mkdirSync, writeFileSync } from "fs";

// Refuse to run against a robot that's connected, and make sure our build is the one answering.
export default async function globalSetup() {
  const base = process.env.PV_URL ?? "http://localhost:5800";
  let state: Response;
  try {
    state = await fetch(`${base}/api/spectrum/uiState`);
  } catch (e) {
    throw new Error(
      `Can't reach PhotonVision at ${base}. Open the tunnel first (tests/ui/run.sh does it), or set PV_URL.`
    );
  }
  if (!state.ok) {
    throw new Error(
      `${base}/api/spectrum/uiState answered ${state.status}: this PhotonVision build is older than photonvision-45.`
    );
  }
  // Every camera's running pipeline, so global-teardown can catch a test that leaves one moved.
  const cameras = (await state.json()).cameras as { nickname: string; currentPipelineIndex: number; pipelineNicknames: string[] }[];
  const start = cameras.map((c) => ({ camera: c.nickname, pipeline: c.pipelineNicknames[c.currentPipelineIndex] ?? String(c.currentPipelineIndex) }));
  mkdirSync(".state", { recursive: true });
  writeFileSync(".state/start-pipelines.json", JSON.stringify(start, null, 2));
  const rewind = await (await fetch(`${base}/api/rewind`)).json();
  if (rewind.robotConnected && process.env.PV_UI_TEST_ON_ROBOT !== "1") {
    throw new Error(
      "The Jetson is connected to a robot. These tests switch pipelines and move camera settings; " +
        "run them on the bench, or set PV_UI_TEST_ON_ROBOT=1 if the robot is safe (disabled, on blocks)."
    );
  }
}

import { expect, type APIRequestContext } from "@playwright/test";
import { spawn, type ChildProcess } from "child_process";

// Runs tests/fake-robot on the Jetson over SSH (run.sh sets PV_UI_JETSON). The fake robot and its
// script have their own deadlines; stop() ends it early through its stop file.

export const JETSON = process.env.PV_UI_JETSON;

function ssh(command: string): ChildProcess {
  return spawn(
    "ssh",
    ["-o", "BatchMode=yes", "-i", `${process.env.HOME}/.ssh/jetson_ed25519`, `spectrum3847@${JETSON}`, command],
    { stdio: "ignore" }
  );
}

export interface RobotState {
  idleWhileDisabled: boolean;
  idleFps: number;
  robotConnected: boolean;
  enabled: boolean;
  fmsAttached: boolean;
  idleNow: boolean;
  eventProfileOnFms: boolean;
  eventPipeline: number;
  lastEventSwitch: string;
  farSearch: boolean;
  farSweepsPerSecond: number;
}

export async function robotState(request: APIRequestContext): Promise<RobotState> {
  return (await (await request.get("/api/robotState")).json()) as RobotState;
}

export class FakeRobot {
  private constructor(private readonly process: ChildProcess, private readonly request: APIRequestContext) {}

  /** Phases as for tests/fake-robot/run.sh, e.g. "disabled:5", "fms-disabled:30". */
  static async start(request: APIRequestContext, ...phases: string[]): Promise<FakeRobot> {
    const robot = new FakeRobot(ssh(`bash ~/SpectrumJetson/tests/fake-robot/run.sh ${phases.join(" ")}`), request);
    await expect
      .poll(async () => (await robotState(request)).robotConnected, { timeout: 30_000, message: "PhotonVision connected to the fake robot" })
      .toBe(true);
    return robot;
  }

  async stop(): Promise<void> {
    ssh("touch /tmp/fake-robot-stop");
    await expect.poll(async () => (await robotState(this.request)).robotConnected, { timeout: 15_000 }).toBe(false);
    this.process.kill();
  }
}

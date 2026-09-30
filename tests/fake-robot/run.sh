#!/usr/bin/env bash
# Bench test for PhotonVision's robot-state features with a fake robot. Run ON THE JETSON:
#   tests/fake-robot/run.sh                                   # disabled 30 s, enabled 30 s, disabled 30 s
#   tests/fake-robot/run.sh disabled:20 fms-disabled:20 ...   # any phases (see FakeRobot.java)
# PhotonVision looks for team 8515's robot at 10.85.15.2 among other addresses, so for the length
# of the test the loopback interface gets that address and FakeRobot.java (a NetworkTables server)
# answers there: no PhotonVision setting changes. Removed again on exit. Refuses to run on a robot.
# Prints, per phase: each camera's frames a second, board power, and the garbage-collection log.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
JAVA=/usr/lib/jvm/java-17-openjdk-arm64/bin/java
PHASES=("$@")
[[ ${#PHASES[@]} -gt 0 ]] || PHASES=(disabled:30 enabled:30 disabled:30)
# A hard deadline for the whole run: the phases, plus 60 s to connect and report. On a timeout
# the cleanup below still runs (loopback address removed, tegrastats stopped).
if [[ -z ${FAKE_ROBOT_UNDER_TIMEOUT:-} ]]; then
  total=$(python3 -c 'import sys; print(int(sum(float(p.split(":")[1]) for p in sys.argv[1:])) + 60)' "${PHASES[@]}")
  rc=0
  FAKE_ROBOT_UNDER_TIMEOUT=1 timeout --kill-after=10 "$total" "$0" "${PHASES[@]}" || rc=$?
  [[ $rc == 124 ]] && echo "TIMEOUT: the fake-robot test didn't finish in $total s" >&2
  exit "$rc"
fi
ROBOT_IP=10.85.15.2
LOG=/tmp/fake-robot.log
TEGRA=/tmp/fake-robot-tegrastats.log

connected=$(python3 -c 'import json,urllib.request;print(json.load(urllib.request.urlopen("http://localhost:5800/api/rewind"))["robotConnected"])')
[[ $connected == False ]] || { echo "PhotonVision is connected to a robot already; not running." >&2; exit 1; }

added=0
cleanup() {
  [[ -n ${tegra_pid:-} ]] && sudo tegrastats --stop 2>/dev/null || true
  [[ -n ${robot_pid:-} ]] && kill "$robot_pid" 2>/dev/null || true
  [[ $added == 1 ]] && sudo ip addr del "$ROBOT_IP/32" dev lo 2>/dev/null || true
}
trap cleanup EXIT
if ! ip -4 addr show dev lo | grep -q "$ROBOT_IP/"; then
  sudo ip addr add "$ROBOT_IP/32" dev lo
  added=1
fi

start=$(date +%s)
sudo rm -f "$TEGRA"
sudo tegrastats --interval 1000 --logfile "$TEGRA" >/dev/null 2>&1 &
tegra_pid=$!
"$JAVA" -cp /opt/photonvision/photonvision.jar "$HERE/FakeRobot.java" "${PHASES[@]}" 2>&1 | grep -v "^\[" > "$LOG" &
robot_pid=$!
echo "Fake robot at $ROBOT_IP: ${PHASES[*]}"
wait "$robot_pid" || true
robot_pid=""
# Killing sudo leaves tegrastats running (and holding this script's output open): --stop ends it.
sudo tegrastats --stop 2>/dev/null || true
tegra_pid=""
sleep 1

journalctl -u photonvision --since "@$start" --no-pager -o short-unix > /tmp/fake-robot-pv.log
python3 - "$LOG" "$TEGRA" /tmp/fake-robot-pv.log <<'PY'
import re, sys
robot, tegra, pv = sys.argv[1:4]
phases, done = [], None
for line in open(robot):
    m = re.match(r"phase (\d+) (\S+) (\d+)", line)
    if m: phases.append((m.group(2), int(m.group(3)) / 1000))
    m = re.match(r"done (\d+)", line)
    if m: done = int(m.group(1)) / 1000
    if line.startswith("connected"): print("  PhotonVision connected to the fake robot")
if not phases:
    print("  PhotonVision never connected to the fake robot"); sys.exit(1)
bounds = [(s, t, (phases[i + 1][1] if i + 1 < len(phases) else done)) for i, (s, t) in enumerate(phases)]

stats = []  # (time, handle, calls/s)
events = []
for line in open(pv):
    t = float(line.split()[0])
    m = re.search(r"971 stats (h\d+) \S+ ([\d.]+) calls/s", line)
    if m: stats.append((t, m.group(1), float(m.group(2))))
    if "garbage collected" in line or "Idle mode" in line:
        events.append((t, line.split("] ")[-1].strip()))

# tegrastats lines have no timestamps; one a second from the start of the fake robot's run
power = [int(m.group(1)) for m in (re.search(r"VDD_IN (\d+)mW", l) for l in open(tegra)) if m]
t0 = phases[0][1] - 2  # FakeRobot waits 2 s after connecting

for state, a, b in bounds:
    # Skip the first 3 s of a phase (the 1 s stats window straddles the change).
    rows = [s for s in stats if a + 3 <= s[0] <= b]
    per = {}
    for _, h, fps in rows: per.setdefault(h, []).append(fps)
    fps = ", ".join(f"{h} {sum(v) / len(v):.0f}" for h, v in sorted(per.items()))
    watts = power[int(a + 3 - t0): int(b - t0)]
    w = f"{sum(watts) / len(watts) / 1000:.1f} W" if watts else "- W"
    print(f"  {state:14s} {b - a:4.0f} s   fps: {fps}   board {w}")
for t, e in events:
    print(f"  +{t - phases[0][1]:5.1f} s  {e}")
PY

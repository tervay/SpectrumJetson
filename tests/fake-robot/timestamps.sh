#!/usr/bin/env bash
# The capture timestamps robot code receives, per camera: interval and jitter. Run ON THE JETSON:
#   tests/fake-robot/timestamps.sh [seconds, default 30]
# Same fake-robot address trick as run.sh (10.85.15.2 on lo for the run). Refuses on a robot.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
S=${1:-30}
if [[ -z ${TS_UNDER_TIMEOUT:-} ]]; then
  rc=0
  TS_UNDER_TIMEOUT=1 timeout --kill-after=10 $((S + 60)) "$0" "$@" || rc=$?
  [[ $rc == 124 ]] && echo "TIMEOUT: timestamps.sh didn't finish" >&2
  exit "$rc"
fi
connected=$(python3 -c 'import json,urllib.request;print(json.load(urllib.request.urlopen("http://localhost:5800/api/rewind"))["robotConnected"])')
[[ $connected == False ]] || { echo "PhotonVision is connected to a robot already; not running." >&2; exit 1; }
added=0
cleanup() { [[ $added == 1 ]] && sudo ip addr del 10.85.15.2/32 dev lo 2>/dev/null || true; }
trap cleanup EXIT
if ! ip -4 addr show dev lo | grep -q "10.85.15.2/"; then sudo ip addr add 10.85.15.2/32 dev lo; added=1; fi
/usr/lib/jvm/java-17-openjdk-arm64/bin/java -cp /opt/photonvision/photonvision.jar "$HERE/ResultTimestamps.java" "$S" 2>&1 | grep -vE "^\[|^NT:"

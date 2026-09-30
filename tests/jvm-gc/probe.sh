#!/usr/bin/env bash
# PhotonVision's GC and safepoint log, switched on live (no restart), next to each detector's
# per-second worst detect time. Run ON THE JETSON: tests/jvm-gc/probe.sh [seconds, default 120]
# Then: python3 tests/jvm-gc/analyze.py /tmp/pv-gc-copy.log /tmp/pv-stats.log
set -euo pipefail
J=/usr/lib/jvm/java-17-openjdk-arm64/bin/jcmd
P=$(systemctl show photonvision -p MainPID --value)
OUT=/tmp/pv-gc.log
sudo rm -f $OUT
start=$(date +%s)
sudo timeout 20 $J $P VM.log output=file=$OUT what=gc,safepoint decorators=uptime,time >/dev/null
sleep ${1:-120}
sudo timeout 20 $J $P VM.log output=file=$OUT what=disable >/dev/null || true
end=$(date +%s)
sudo cp $OUT /tmp/pv-gc-copy.log; sudo chown $USER /tmp/pv-gc-copy.log
journalctl -u photonvision --since "@$start" --until "@$end" --no-pager -o short-unix | grep "971 stats" > /tmp/pv-stats.log
echo "window $((end-start)) s"

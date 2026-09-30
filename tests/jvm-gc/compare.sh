#!/usr/bin/env bash
# Try PhotonVision JVM flags: restart with them, 45 s warm-up, then SECONDS of GC log and detect
# stats, CPU and memory. Puts the service file back afterwards (PhotonVision restarts again on the
# next start). Run ON THE JETSON:
#   tests/jvm-gc/compare.sh LABEL "JVM FLAGS" [seconds, default 120]
#   e.g. tests/jvm-gc/compare.sh zgc "-Xmx1g -XX:+UseZGC" 360
# A young collection only shows after its young generation fills: with a 512 MB young generation,
# run over 3 minutes (see docs/TECHNICAL.md, "Garbage collection").
set -euo pipefail
LABEL=$1; FLAGS=$2
HERE=$(cd "$(dirname "$0")" && pwd)
# Hard deadline: warm-up + the run + 90 s; the trap still puts the service file back.
if [[ -z ${GC_COMPARE_UNDER_TIMEOUT:-} ]]; then
  rc=0
  GC_COMPARE_UNDER_TIMEOUT=1 timeout --kill-after=10 $(( 45 + ${3:-120} + 90 )) "$0" "$@" || rc=$?
  [[ $rc == 124 ]] && echo "TIMEOUT: compare.sh didn't finish" >&2
  exit "$rc"
fi
CONF=/etc/systemd/system/photonvision.service.d/java17.conf
sudo cp $CONF /tmp/java17.conf.orig
restore() { sudo cp /tmp/java17.conf.orig $CONF; sudo systemctl daemon-reload; }
trap restore EXIT
sudo sed -i "s|-Xmx512m|$FLAGS|" $CONF
grep ExecStart= $CONF | tail -1
sudo systemctl daemon-reload; sudo systemctl restart photonvision
sleep 45
P=$(systemctl show photonvision -p MainPID --value)
t0=$(awk '{print $14+$15}' /proc/$P/stat); s0=$(date +%s.%N)
bash "$HERE/probe.sh" ${3:-120} >/dev/null
t1=$(awk '{print $14+$15}' /proc/$P/stat); s1=$(date +%s.%N)
rss=$(awk '/VmRSS/{print int($2/1024)}' /proc/$P/status)
cores=$(python3 -c "print(round(($t1-$t0)/$(getconf CLK_TCK)/($s1-$s0),2))")
echo "== $LABEL: PhotonVision $cores cores, RSS $rss MB"
python3 "$HERE/analyze.py" /tmp/pv-gc-copy.log /tmp/pv-stats.log | grep -vE "^h[0-9]"
python3 "$HERE/analyze.py" /tmp/pv-gc-copy.log /tmp/pv-stats.log | grep -E "^h[0-9]" | awk '{print "  "$0}' | cut -c1-110

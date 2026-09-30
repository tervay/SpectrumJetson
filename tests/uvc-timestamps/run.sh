#!/usr/bin/env bash
# Driver timestamps (first USB packet) against the camera's own clock (uvcvideo hwtimestamps=1,
# the camera's PTS converted to the host clock through its SCR). Run ON THE JETSON; stops
# PhotonVision for about a minute and starts it again.
#   tests/uvc-timestamps/run.sh [device] [frames]
# For each mode, per frame: the buffer's timestamp, and the host's CLOCK_MONOTONIC when v4l2-ctl
# printed the frame (just after it was dequeued). Prints how far back each stamp is from the
# dequeue, and the frame-to-frame jitter.
set -euo pipefail
# "all" = every camera at once (USB contention, as on the robot).
DEV=${1:-/dev/v4l/by-path/platform-3610000.usb-usb-0:2.1:1.0-video-index0}
if [[ $DEV == all ]]; then DEVS=(/dev/v4l/by-path/*usb-0:*-video-index0); else DEVS=("$DEV"); fi
N=${2:-600}
P=/sys/module/uvcvideo/parameters/hwtimestamps
if [[ -z ${UVC_TS_UNDER_TIMEOUT:-} ]]; then
  rc=0
  UVC_TS_UNDER_TIMEOUT=1 timeout --kill-after=10 120 "$0" "$@" || rc=$?
  [[ $rc == 124 ]] && echo "TIMEOUT: the timestamp test didn't finish in 120 s" >&2
  exit "$rc"
fi
old=$(cat $P)
restore() { echo "$old" | sudo tee $P >/dev/null; sudo systemctl start photonvision; }
trap restore EXIT
sudo systemctl stop photonvision
sleep 2
for hw in 0 1; do
  echo $hw | sudo tee $P >/dev/null
  for i in "${!DEVS[@]}"; do
    timeout 30 stdbuf -oL v4l2-ctl -d "${DEVS[$i]}" --set-fmt-video=width=1280,height=800,pixelformat=MJPG \
      --set-parm=120 --stream-mmap --stream-count=$((N + 60)) --verbose 2>&1 \
      | python3 -u -c '
import sys, time
for line in sys.stdin:
    if " ts: " in line:
        print(time.clock_gettime(time.CLOCK_MONOTONIC), line.strip())
' > /tmp/uvc-ts-$hw-$i.log &
  done
  wait || true
done
for i in "${!DEVS[@]}"; do
echo "== ${DEVS[$i]##*usb-0:}"
python3 - /tmp/uvc-ts-0-$i.log /tmp/uvc-ts-1-$i.log <<'PY'
import re, sys, statistics as st
for path, name in zip(sys.argv[1:], ["driver (first USB packet)", "camera clock (hwtimestamps=1)"]):
    rows = []
    flags = ""
    for line in open(path):
        m = re.match(r"([\d.]+) .*ts: ([\d.]+)", line)
        if m: rows.append((float(m.group(1)), float(m.group(2))))
        f = re.search(r"\(([^)]*ts-[^)]*)\)", line)
        if f: flags = f.group(1)
    rows = rows[60:]  # skip the start-up frames
    if len(rows) < 10:
        print(f"{name}: too few frames"); continue
    back = [(a - t) * 1000 for a, t in rows]
    d = [(rows[i][1] - rows[i - 1][1]) * 1000 for i in range(1, len(rows))]
    print(f"{name}: {len(rows)} frames, flags ({flags})")
    print(f"  stamp is {st.median(back):.2f} ms before the dequeue (p5 {sorted(back)[len(back)//20]:.2f}, p95 {sorted(back)[len(back)*19//20]:.2f})")
    print(f"  frame interval {st.mean(d):.3f} ms, jitter (std) {st.pstdev(d):.3f} ms, min {min(d):.2f}, max {max(d):.2f}")
PY
done

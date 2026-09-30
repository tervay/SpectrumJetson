#!/usr/bin/env bash
# The far-tag search on recordings (detector/far_replay). Run ON THE JETSON.
#   tests/far-search/replay.sh                 # check far_replay on a synthetic session (known answer)
#   tests/far-search/replay.sh SESSION_DIR...  # then replay real Rewind sessions, far search on and off
# Extra far_replay options (--calib CAMERA=..., --budget N, --every N) go in FAR_REPLAY_ARGS.
# The check: a synthetic session (far_search_test --write-session) where CamA sees near tags for the
# first and last second and a far 14 px tag all along. far_replay must find the far tag on frames the
# normal search misses (only while no camera has a good view), and nothing extra with --off.
set -uo pipefail
OUT=${OUT:-$HOME/build/bos-detector}
[[ -f $OUT/build.ninja ]] || { echo "Build the detector first: scripts/jetson/07-build-bos-detector.sh" >&2; exit 1; }
timeout 900 cmake --build "$OUT" --parallel 4 --target far_search_test far_replay >/dev/null || exit 1
quiet() { grep -vE "^E0000|^WARNING: All log|far search: (on|off)|^  \.\.\."; }
fails=0
check() { if eval "$2"; then echo "  PASS  $1"; else echo "  FAIL  $1"; fails=$((fails + 1)); fi; }

S=$(mktemp -d /tmp/far-replay-check.XXXX)
trap 'rm -rf "$S"' EXIT
timeout 300 "$OUT/far_search_test" --write-session "$S/session" | quiet
on=$(timeout 300 "$OUT/far_replay" "$S/session" --out "$S/on.csv" 2>&1 | quiet)
off=$(timeout 300 "$OUT/far_replay" "$S/session" --off 2>&1 | quiet)
echo "== far search on"; echo "$on"; echo "== off"; echo "$off" | grep -E "Total|CamA|CamB"
far7=$(sed -n 's/.*tag 7: \([0-9]*\) frames only the far search.*/\1/p' <<<"$on")
check "the far search finds the far tag on frames the normal search misses (${far7:-0} frames)" '[[ ${far7:-0} -ge 10 ]]'
check "with --off, nothing extra" 'grep -q "(+0.0%)" <<<"$off"'
# Far detections only in the middle second (1.25-2.0 s after the start: no good view).
bad=$(awk -F, 'NR > 1 && $7 == "far" && ($3 < 2250000 || $3 > 3000000) {n++} END {print n + 0}' "$S/on.csv")
check "far detections only while no camera has a good view ($bad outside it)" '[[ $bad -eq 0 ]]'

for session in "$@"; do
  echo; echo "== $session"
  # A deadline that fits the recording (replay runs at ~150 frames a second): 60 s plus 1 s per
  # 100 frames. A generous flat one hid a stalled frame for 6 minutes once (bos-08).
  frames=$(cat "$session"/*/[0-9]*.csv 2>/dev/null | grep -c '^[0-9]')
  limit=$((60 + frames / 100))
  rc=0
  timeout "$limit" "$OUT/far_replay" "$session" ${FAR_REPLAY_ARGS:-} --out "/tmp/far-$(basename "$session").csv" 2>&1 | quiet || rc=${PIPESTATUS[0]}
  if [[ $rc == 124 ]]; then
    echo "  FAIL  TIMEOUT after ${limit} s ($frames frames): run far_replay with --trace to find the frame"
    fails=$((fails + 1))
  fi
done
echo
[[ $fails -eq 0 ]] && echo "PASS" || echo "FAIL: $fails check(s)"
exit $((fails > 0))

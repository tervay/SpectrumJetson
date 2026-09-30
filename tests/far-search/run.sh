#!/usr/bin/env bash
# Bench test for the far-tag search (detector/far_search.h). Run ON THE JETSON; no camera needed,
# PhotonVision can keep running. Builds detector/far_search_test in the detector's build folder and
# runs it: synthetic tag36h11 tags in 1280x800 frames check the range gain, corner agreement, the
# good-view / starved policy, tracking, the budget and the off switch. Exit status 0 = all pass.
set -euo pipefail
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$HOME/build/bos-detector}
[[ -f $OUT/build.ninja ]] || { echo "Build the detector first: scripts/jetson/07-build-bos-detector.sh" >&2; exit 1; }
timeout 900 cmake --build "$OUT" --parallel 4 --target far_search_test >/dev/null
rc=0
timeout --kill-after=10 600 "$OUT/far_search_test" 2>&1 | grep -vE "^E0000|^WARNING: All log|far search: (on|off)" || rc=${PIPESTATUS[0]}
[[ $rc == 124 ]] && echo "TIMEOUT: far_search_test didn't finish in 10 min" >&2
exit "$rc"

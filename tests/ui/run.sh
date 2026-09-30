#!/usr/bin/env bash
# Run the PhotonVision browser tests from the laptop against the Jetson.
#   tests/ui/run.sh                 # all tests
#   tests/ui/run.sh round-trip      # only specs whose file name matches
#   PV_UI_CAMERA=TopRight tests/ui/run.sh
#   PV_UI_HEADED=1 tests/ui/run.sh  # watch the browser
# Opens an SSH tunnel to the Jetson's ports 5800 and 1181-1200 (streams) (USB link first, then Wi-Fi) unless something
# is already listening on localhost:5800. Uses the laptop's Chrome and the Node from
# scripts/host/03-build-photonvision-fork.sh (~/build/tools/node).
set -euo pipefail
cd "$(dirname "$0")"
export PATH=$HOME/build/tools/node/bin:$PATH
KEY=${JETSON_KEY:-$HOME/.ssh/jetson_ed25519}
HOSTS=${JETSON_HOSTS:-"192.168.55.1 10.100.0.194"}

command -v node >/dev/null || { echo "No Node: run scripts/host/03-build-photonvision-fork.sh once" >&2; exit 1; }
[[ -d node_modules/@playwright/test ]] || npm install --no-audit --no-fund

# PhotonVision on 5800, and the camera streams on 1181 and up (two per camera).
FORWARDS=(-L 5800:localhost:5800)
for port in $(seq 1181 1200); do FORWARDS+=(-L "$port:localhost:$port"); done

tunnel_pid=""
cleanup() { [[ -n $tunnel_pid ]] && kill "$tunnel_pid" 2>/dev/null || true; }
trap cleanup EXIT
if ! (exec 3<>/dev/tcp/127.0.0.1/5800) 2>/dev/null; then
  for host in $HOSTS; do
    ssh -q -N -o ExitOnForwardFailure=yes -o ConnectTimeout=4 -o ServerAliveInterval=15 -o BatchMode=yes \
      -i "$KEY" "${FORWARDS[@]}" "spectrum3847@$host" &
    tunnel_pid=$!
    for _ in $(seq 20); do
      (exec 3<>/dev/tcp/127.0.0.1/5800) 2>/dev/null && break 2
      kill -0 "$tunnel_pid" 2>/dev/null || break
      sleep 0.5
    done
    kill "$tunnel_pid" 2>/dev/null || true
    tunnel_pid=""
  done
  [[ -n $tunnel_pid ]] || { echo "Couldn't open a tunnel to the Jetson ($HOSTS)" >&2; exit 1; }
  export PV_UI_JETSON=$host
  echo "Tunnel to the Jetson open on localhost:5800"
fi

# Tests that run something on the Jetson (the fake robot) need its address.
if [[ -z ${PV_UI_JETSON:-} ]]; then
  for host in $HOSTS; do
    ssh -o ConnectTimeout=4 -o BatchMode=yes -i "$KEY" "spectrum3847@$host" true 2>/dev/null && { export PV_UI_JETSON=$host; break; }
  done
fi

# Belt and braces over Playwright's own globalTimeout (8 min): nothing here runs past 10 min.
rc=0
timeout --kill-after=10 600 npx playwright test "$@" || rc=$?
[[ $rc == 124 ]] && echo "TIMEOUT: the browser tests didn't finish in 10 min" >&2
exit "$rc"

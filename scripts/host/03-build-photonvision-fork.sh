#!/usr/bin/env bash
# Build the FRC-Team-4143 PhotonVision fork (CUDA AprilTag pipeline) as a linuxarm64 jar,
# on the x86-64 host. Mise manages Node 22, pnpm 10 and Temurin 17 without sudo.
# The patched v2026.3.4 server and native detector use WPILib 2026.2.1.
set -euo pipefail

FORK_URL=https://github.com/FRC-Team-4143/photonvision.git
FORK_SHA=d8c9e8e1d3d4036f077a92f10f3a513ef4849765   # jetson-orin == main, 2026-01-30
BUILD=$HOME/build
SRC=$BUILD/photonvision-4143
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$REPO_ROOT/out

command -v mise >/dev/null || { echo "Install Mise before running this build." >&2; exit 1; }
mise -C "$REPO_ROOT" install java node pnpm
# Keep the project tool versions after changing to the external source checkout.
mise_env=$(mise -C "$REPO_ROOT" env --shell bash)
eval "$mise_env"
mkdir -p "$OUT"
echo "node $(node --version), pnpm $(pnpm --version), $(java -version 2>&1 | head -1)"

if [[ ! -d $SRC/.git ]]; then
  git clone --filter=blob:none "$FORK_URL" "$SRC"
fi
cd "$SRC"
git fetch -q origin
# Reset to the pinned commit, then apply our patches (see patches/).
git checkout -q -f "$FORK_SHA"
git reset -q --hard "$FORK_SHA"
git clean -fdq   # drop files created by earlier patch runs (ignored build caches stay)
for p in "$REPO_ROOT"/patches/photonvision-*.patch; do
  [[ -e $p ]] || continue
  echo "==> Applying $(basename "$p")"
  git apply "$p"
done
# The 3D field models for the Field Calibration page (photonvision-30), from assets/field-models
# (built by tools/fieldmodel from FIRST's field CAD): served by PhotonVision at fieldmodels/.
mkdir -p photon-client/public/fieldmodels
cp "$REPO_ROOT"/assets/field-models/*.glb photon-client/public/fieldmodels/
# The training site (training/) is the sidebar's Documentation page: DocsView frames docs/index.html.
# Upstream CI fills docs/ with its Sphinx manual; we ship our course instead (works offline).
mkdir -p photon-client/public/docs
tar -C "$REPO_ROOT/training" --exclude=tools --exclude=README.md --exclude=.nojekyll -cf - . \
  | tar -C photon-client/public/docs -xf -
# Upstream tags give the jar a sane version string (e.g. v2026.1.1-27-gd8c9e8e1).
git fetch -q --tags https://github.com/PhotonVision/photonvision.git || true

./gradlew installArm64Toolchain
# Mise supplies pnpm; skip the Node plugin's unpinned npm-global installation.
./gradlew photon-targeting:jar photon-server:shadowJar -PArchOverride=linuxarm64 \
  -x :photon-server:pnpmSetup

jar=$(ls -t photon-server/build/libs/photonvision-*-linuxarm64.jar | head -1)
cp "$jar" "$OUT/"
echo
echo "Built: $OUT/$(basename "$jar")"

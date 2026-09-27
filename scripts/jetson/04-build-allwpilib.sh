#!/usr/bin/env bash
# Build and install allwpilib natively on the Jetson, for GpuDetectorJNI to link against.
# Run ON THE JETSON, ideally inside tmux (this takes hours):
#   tmux new -s build ~/SpectrumJetson/scripts/jetson/04-build-allwpilib.sh
#
# The tag must match the wpilibVersion of the PhotonVision build that loads
# lib971apriltag.so. The FRC-Team-4143/photonvision fork (d8c9e8e) uses 2026.2.1.
# allwpilib main does NOT work: wpi/jni_util.h has moved there.
# Usage: 04-build-allwpilib.sh [tag]
set -euo pipefail

TAG=${1:-v2026.2.1}
SRC=$HOME/build/allwpilib-$TAG
LOG=$HOME/build/allwpilib-$TAG.log
# More parallelism OOMs on wpimath with 8 GB RAM.
JOBS=4

# Pin JDK 17 explicitly instead of relying on the system's Java alternatives.
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-arm64
export PATH=$PATH:/usr/local/cuda/bin

sudo -v
# Keep sudo fresh so the final install doesn't block on a password hours from now.
( while true; do sudo -n true; sleep 60; done ) 2>/dev/null &
KEEPALIVE=$!
trap 'kill $KEEPALIVE 2>/dev/null' EXIT

sudo apt-get install -y openjdk-17-jdk ninja-build protobuf-compiler libxrandr-dev \
  libssh-dev libopencv4.5-java cmake build-essential git

mkdir -p "$HOME/build"
if [[ ! -d $SRC/.git ]]; then
  git clone --depth 1 --branch "$TAG" https://github.com/wpilibsuite/allwpilib.git "$SRC"
fi
cd "$SRC"

# A directory name alone does not establish which sources are being built.
expected_commit=$(git rev-parse --verify "refs/tags/$TAG^{commit}")
if [[ $(git rev-parse HEAD) != "$expected_commit" ]] ||
   [[ -n $(git status --porcelain --untracked-files=no) ]]; then
  echo "Refusing to build $SRC: expected a clean checkout of $TAG." >&2
  exit 1
fi

exec > >(tee -a "$LOG") 2>&1
echo "==> $(date) building allwpilib $TAG with -j$JOBS (log: $LOG)"

cmake --preset default -DWITH_GUI=OFF -DWITH_JAVA=ON \
  -DWITH_SIMULATION_MODULES=OFF -DWITH_TESTS=OFF \
  -DOPENCV_JAR_FILE=/usr/share/java/opencv.jar

cd build-cmake
time cmake --build . --parallel "$JOBS"
sudo cmake --build . --target install
sudo ldconfig

echo "==> $(date) allwpilib $TAG installed to /usr/local"

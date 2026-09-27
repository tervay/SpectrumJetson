#!/usr/bin/env bash
# Swap the 4143 CUDA PhotonVision fork jar into the photonvision.service created by
# 03-photonvision.sh, and run it explicitly on Java 17, matching the 2026 build.
# Run ON THE JETSON. Usage: 06-install-fork-jar.sh <path/to/photonvision-...-linuxarm64.jar>
set -euo pipefail

JAR=${1:?usage: $0 <photonvision-linuxarm64.jar>}
JAVA17=/usr/lib/jvm/java-17-openjdk-arm64/bin/java
NET_FLAG=""
[[ ${PV_MANAGE_NETWORK:-1} == 0 ]] && NET_FLAG=" -n"
DEST=/opt/photonvision/photonvision.jar

[[ -x $JAVA17 ]] || { echo "Missing $JAVA17 (sudo apt install openjdk-17-jdk)" >&2; exit 1; }
# Refuse truncated/corrupt jars: a bad jar here leaves the service crash-looping.
# (No pipes here: with pipefail, `unzip -l | grep -q` fails when grep exits early.)
if ! unzip -tq "$JAR" >/dev/null 2>&1 || ! unzip -l "$JAR" org/photonvision/Main.class >/dev/null 2>&1; then
  echo "Refusing to install $JAR: not a valid PhotonVision jar ($(stat -c %s "$JAR") bytes)." >&2
  exit 1
fi
[[ -f /usr/lib/lib971apriltag.so ]] || echo "WARNING: /usr/lib/lib971apriltag.so missing; CUDA pipeline will fail to load." >&2

sudo systemctl stop photonvision

# Keep the jar being replaced, once, so we can roll back.
if [[ -f $DEST && ! -f /opt/photonvision/photonvision.jar.orig ]]; then
  sudo cp "$DEST" /opt/photonvision/photonvision.jar.orig
fi
# Also keep the last *valid* jar as .prev for a one-step rollback.
if [[ -f $DEST ]] && unzip -tq "$DEST" >/dev/null 2>&1; then
  sudo cp "$DEST" /opt/photonvision/photonvision.jar.prev
fi
sudo install -m 644 "$JAR" "$DEST"

# Drop-in override instead of editing the installer's unit.
# PhotonVision manages networking (static IP, hostname) from its UI unless
# PV_MANAGE_NETWORK=0, which passes -n (--disable-networking). Before enabling it,
# make sure PV's Hostname field holds the name you want: PV applies it to the system.
# -XX:-CreateCoredumpOnCrash: a native crash otherwise dumps core through Apport, which took
# ~28 s (and 156 MB) before systemd could restart PhotonVision.
sudo mkdir -p /etc/systemd/system/photonvision.service.d
sudo tee /etc/systemd/system/photonvision.service.d/java17.conf >/dev/null <<EOF
[Service]
ExecStart=
ExecStart=$JAVA17 -Xmx512m -XX:-CreateCoredumpOnCrash -jar $DEST$NET_FLAG
EOF

sudo systemctl daemon-reload
sudo systemctl start photonvision
sleep 10
systemctl --no-pager --lines=0 status photonvision || true
journalctl -u photonvision --no-pager -n 300 | grep -iE "version|971|cuda|jetson|exception|error" | tail -15 || true

echo
echo "Rollback: sudo cp /opt/photonvision/photonvision.jar.orig $DEST && \\"
echo "  sudo rm /etc/systemd/system/photonvision.service.d/java17.conf && \\"
echo "  sudo systemctl daemon-reload && sudo systemctl restart photonvision"

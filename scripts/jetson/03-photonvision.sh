#!/usr/bin/env bash
# Install upstream PhotonVision (CPU AprilTag detection) as a boot-time systemd service.
# Run ON THE JETSON. Needs internet.
#
# Use stable 2026 PhotonLib in the robot code.
# Usage: scripts/jetson/03-photonvision.sh [version]
set -euo pipefail

PV_VERSION=${1:-v2026.3.4}
# Last installer revision before the Java 25 / 2027 transition.
INSTALLER_SHA=e5018c1f8e2045c46ba90bb3543b43d0a866e066
INSTALLER_URL=https://raw.githubusercontent.com/PhotonVision/photon-image-modifier/$INSTALLER_SHA/install.sh

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
wget -q -O "$tmp/install.sh" "$INSTALLER_URL"

# --no-networking: leave NetworkManager config alone (the service runs with -n).
# Use the long --version= form; the installer's short -v does not take an argument reliably.
sudo bash "$tmp/install.sh" --version="$PV_VERSION" --no-networking

# Select Java 17 even if another runtime is the system default. The fork installer
# replaces this same drop-in when it installs the customized jar.
sudo mkdir -p /etc/systemd/system/photonvision.service.d
sudo tee /etc/systemd/system/photonvision.service.d/java17.conf >/dev/null <<'EOF'
[Service]
ExecStart=
ExecStart=/usr/lib/jvm/java-17-openjdk-arm64/bin/java -Xmx512m -XX:-CreateCoredumpOnCrash -jar /opt/photonvision/photonvision.jar -n
EOF
sudo systemctl daemon-reload

sudo systemctl restart photonvision
sleep 5
systemctl --no-pager --lines=0 status photonvision || true

echo
echo "PhotonVision $PV_VERSION installed. Web UI: http://<jetson-ip>:5800"
echo "In the UI, set Settings > Networking > Team Number (8515 for the Oct 2026 event)."

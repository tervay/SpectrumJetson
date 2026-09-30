#!/usr/bin/env bash
# Robot-readiness tuning for the Jetson. Run ON THE JETSON; asks for sudo once.
# Safe to re-run. Reboot afterwards to get the boot-time and persistence benefits.
#
#   1. No automatic updates   - apt must not update NVIDIA/Java packages mid-event
#   2. snapd off              - its first-boot "seeding" took 45 s of every boot (no snaps used)
#   3. Headless               - boot to multi-user (no GNOME desktop on a robot)
#   4. Clocks locked at boot  - jetson_clocks after nvpmodel, before PhotonVision
#                               (MAXN SUPER persists; jetson_clocks does not)
#   5. No USB autosuspend     - for every UVC camera, so cameras never suspend/reconnect
#   6. Power-cut safety       - the robot is switched off, never shut down:
#                               written data reaches the SSD within ~3 s (default up to 30 s), and
#                               the system log is kept on the SSD, synced every 5 s (default: RAM
#                               only, so it vanished at every power cut, taking brownout clues).
#                               ext4's journal keeps the filesystem itself consistent.
#   7. Fan: quiet by default  - NVIDIA's fan control (nvfancontrol) on its "quiet" profile, which
#                               speeds the fan up as the chip warms (~2000 rpm at 56 C).
#                               FAN=full 09-robot-tuning.sh runs it at full speed instead
#                               (jetson_clocks --fan, ~5,800 rpm).
#   8. Recover from hangs     - hardware watchdog 30 s (NVIDIA's default 2 min), also while
#                               rebooting (default 10 min), kernel panic -> reboot in 3 s (default:
#                               hang forever), PhotonVision restarted on any exit (default: only on
#                               failure), with no restart limit
#   9. OpenCV threads sleep   - OpenCV's worker pool busy-waits between jobs by default; its only
#                               jobs here are tiny (preview-stream resize/colour), so 5 workers
#                               spun at ~7% each for nothing. Active waiting off; pool kept.
#  10. USB retries 1 s        - a camera that stops answering (e.g. a Thriftiest after a hub
#                               reset) holds up every other camera on its hub while the kernel
#                               retries it: ~65 s each with the default 5 s timeout. 1 s cuts that
#                               to seconds. A healthy device answers in milliseconds.
#  11. Drop vanished peers    - TCP gives up on a peer that stopped answering after ~13 s
#                               (tcp_retries2 5) instead of ~15 min (15). A dashboard whose laptop
#                               left without closing (lid shut, cable pulled, Wi-Fi gone, driver
#                               station swapped) kept its camera streams "watched", so PhotonVision
#                               kept resizing and encoding frames for nobody. Also applies to NT
#                               (reconnects by itself) and SSH (a >13 s outage with data waiting
#                               drops the session).
#
# Usage: [FAN=quiet|full] 09-robot-tuning.sh [--undo]   (FAN defaults to quiet)
set -euo pipefail

APT_CONF=/etc/apt/apt.conf.d/99spectrum-no-auto-updates
CLOCKS_UNIT=/etc/systemd/system/jetson-clocks.service
UDEV_RULE=/etc/udev/rules.d/90-spectrum-camera-power.rules
SNAP_UNITS=(snapd.service snapd.socket snapd.seeded.service)
SYSCTL_CONF=/etc/sysctl.d/90-spectrum-writeback.conf
JOURNALD_CONF=/etc/systemd/journald.conf.d/90-spectrum-persistent.conf
# zz-: systemd reads system.conf.d in name order and the last setting wins; NVIDIA's own
# watchdog.conf (RuntimeWatchdogSec=120) must come first.
WATCHDOG_CONF=/etc/systemd/system.conf.d/zz-spectrum-watchdog.conf
PANIC_CONF=/etc/sysctl.d/90-spectrum-panic.conf
PV_RESTART_CONF=/etc/systemd/system/photonvision.service.d/90-spectrum-restart.conf
PV_OPENCV_CONF=/etc/systemd/system/photonvision.service.d/90-spectrum-opencv.conf
USB_TMPFILES=/etc/tmpfiles.d/90-spectrum-usb.conf
TCP_CONF=/etc/sysctl.d/90-spectrum-tcp.conf
FAN=${FAN:-quiet}
[[ $FAN == quiet || $FAN == full ]] || { echo "FAN must be quiet or full, not $FAN" >&2; exit 2; }
if [[ $FAN == full ]]; then CLOCKS_ARGS="--fan"; else CLOCKS_ARGS=""; fi

sudo -n true 2>/dev/null || sudo -v   # ask for the password only if sudo needs one

if [[ ${1:-} == --undo ]]; then
  sudo rm -f "$APT_CONF" "$UDEV_RULE" "$SYSCTL_CONF" "$JOURNALD_CONF"
  sudo rm -f "$WATCHDOG_CONF" "$PANIC_CONF" "$PV_RESTART_CONF" "$PV_OPENCV_CONF" "$USB_TMPFILES"
  sudo rm -f "$TCP_CONF"
  sudo sysctl -q net.ipv4.tcp_retries2=15
  echo 5000 | sudo tee /sys/module/usbcore/parameters/initial_descriptor_timeout >/dev/null
  sudo sysctl -q kernel.panic=0
  sudo systemctl daemon-reexec
  sudo systemctl start nvfancontrol || true   # back to NVIDIA's fan control
  sudo sysctl -q vm.dirty_expire_centisecs=3000 vm.dirty_writeback_centisecs=500
  sudo systemctl restart systemd-journald   # logs already on the SSD stay in /var/log/journal
  sudo systemctl enable apt-daily.timer apt-daily-upgrade.timer
  sudo systemctl unmask "${SNAP_UNITS[@]}"
  sudo systemctl enable snapd.service snapd.socket snapd.seeded.service
  sudo systemctl disable jetson-clocks.service 2>/dev/null || true
  sudo rm -f "$CLOCKS_UNIT"
  sudo systemctl set-default graphical.target
  sudo systemctl daemon-reload
  sudo udevadm control --reload
  echo "Undone. Reboot to take effect."
  exit 0
fi

echo "==> 1. Automatic updates off"
sudo systemctl disable --now apt-daily.timer apt-daily-upgrade.timer
sudo tee "$APT_CONF" >/dev/null <<'EOF'
// SpectrumJetson: robot coprocessor, no background package updates.
APT::Periodic::Update-Package-Lists "0";
APT::Periodic::Download-Upgradeable-Packages "0";
APT::Periodic::Unattended-Upgrade "0";
APT::Periodic::AutocleanInterval "0";
EOF

echo "==> 2. snapd off"
# (Already masked on a re-run: then `snap list` waits forever for snapd, so skip it.)
if [[ $(systemctl is-enabled snapd.service 2>/dev/null || true) == masked ]]; then
  echo "    already off"
elif timeout 20 snap list 2>/dev/null | grep -qv "^Name"; then
  echo "    snaps are installed; leaving snapd alone"
else
  sudo systemctl disable --now "${SNAP_UNITS[@]}" 2>/dev/null || true
  sudo systemctl mask "${SNAP_UNITS[@]}"
fi

echo "==> 3. Headless (multi-user.target)"
sudo systemctl set-default multi-user.target

echo "==> 4. jetson_clocks at boot"
sudo tee "$CLOCKS_UNIT" >/dev/null <<EOF
[Unit]
Description=Lock Jetson CPU/GPU/EMC clocks at max for consistent vision latency (SpectrumJetson)
# After nvfancontrol: with --fan (FAN=full) jetson_clocks stops it and sets full speed; if
# nvfancontrol started later it would take the fan back. Without --fan it leaves the fan alone.
After=nvpmodel.service nvfancontrol.service
Before=photonvision.service

[Service]
Type=oneshot
ExecStart=/usr/bin/jetson_clocks $CLOCKS_ARGS
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable --now jetson-clocks.service

echo "==> 5. No USB autosuspend for cameras"
# Any device with a uvcvideo interface: set power/control=on on the USB device (the parent).
sudo tee "$UDEV_RULE" >/dev/null <<'EOF'
# SpectrumJetson: keep UVC cameras powered (no autosuspend -> no dropouts/reconnects).
ACTION=="add|bind", SUBSYSTEM=="usb", DRIVER=="uvcvideo", TEST=="../power/control", ATTR{../power/control}="on"
EOF
sudo udevadm control --reload
# Apply to cameras already plugged in.
for intf in /sys/bus/usb/drivers/uvcvideo/*:*; do
  [[ -e $intf ]] || continue
  dev=$(dirname "$(readlink -f "$intf")")
  echo on | sudo tee "$dev/power/control" >/dev/null
done

echo "==> 6. Power-cut safety"
sudo tee "$SYSCTL_CONF" >/dev/null <<'CONF'
# SpectrumJetson: the robot is switched off, not shut down. Write data to the SSD within ~3 s
# (defaults: 30 s / 5 s), so a power cut loses at most a few seconds.
vm.dirty_expire_centisecs = 300
vm.dirty_writeback_centisecs = 100
CONF
sudo sysctl -q -p "$SYSCTL_CONF"
sudo mkdir -p /var/log/journal "$(dirname "$JOURNALD_CONF")"
sudo tee "$JOURNALD_CONF" >/dev/null <<'CONF'
# SpectrumJetson: keep logs across power cuts (brownout debugging), synced every 5 s, capped.
# 2 GB: with cameras gone, PhotonVision once logged 13 MB a minute (fixed in photonvision-33),
# which at the old 300 MB cap kept only ~25 minutes, less than an event's gap between matches.
[Journal]
Storage=persistent
SyncIntervalSec=5s
SystemMaxUse=2G
CONF
sudo systemd-tmpfiles --create --prefix /var/log/journal
sudo systemctl restart systemd-journald
sudo journalctl --flush

if [[ $FAN == full ]]; then
  echo "==> 7. Fan at full speed (jetson_clocks --fan, in step 4's service)"
  sudo systemctl restart jetson-clocks.service
else
  echo "==> 7. Fan on NVIDIA's quiet profile (nvfancontrol)"
  sudo systemctl restart jetson-clocks.service   # clocks only now
  sudo systemctl enable nvfancontrol
  sudo systemctl restart nvfancontrol
fi

echo "==> 8. Recover from hangs"
sudo mkdir -p "$(dirname "$WATCHDOG_CONF")" "$(dirname "$PV_RESTART_CONF")"
sudo tee "$WATCHDOG_CONF" >/dev/null <<'CONF'
# SpectrumJetson: reboot a hung Jetson after 30 s, not NVIDIA's 2 min (a match is 2:30). The same
# while rebooting (default 10 min): a reboot hung on 2026-09-25 with cameras stuck on the USB hub.
[Manager]
RuntimeWatchdogSec=30s
RebootWatchdogSec=30s
CONF
sudo tee "$PANIC_CONF" >/dev/null <<'CONF'
# SpectrumJetson: reboot 3 s after a kernel panic (default 0: hang until the watchdog fires).
# NVIDIA already sets kernel.panic_on_oops = 1.
kernel.panic = 3
CONF
sudo sysctl -q -p "$PANIC_CONF"
sudo tee "$PV_RESTART_CONF" >/dev/null <<'CONF'
# SpectrumJetson: restart PhotonVision whenever it exits, and never give up.
[Unit]
StartLimitIntervalSec=0
[Service]
Restart=always
RestartSec=1
CONF
sudo systemctl daemon-reexec   # picks up the watchdog setting

echo "==> 9. OpenCV worker threads sleep instead of spinning"
opencv_before=$(cat "$PV_OPENCV_CONF" 2>/dev/null || true)
sudo tee "$PV_OPENCV_CONF" >/dev/null <<'CONF'
# SpectrumJetson: OpenCV's thread pool busy-waits for work by default (2000/10000 spin
# iterations). Its only work in PhotonVision here is small (preview-stream resize and colour
# conversion), so the workers spun for nothing. 0 = sleep until there is work.
[Service]
Environment=OPENCV_THREAD_POOL_ACTIVE_WAIT_WORKER=0
Environment=OPENCV_THREAD_POOL_ACTIVE_WAIT_MAIN=0
CONF
sudo systemctl daemon-reload
if [[ $(cat "$PV_OPENCV_CONF") != "$opencv_before" ]]; then
  echo "    restarting PhotonVision to apply"
  sudo systemctl restart photonvision
fi

echo "==> 10. USB enumeration retries time out after 1 s, not 5 s"
sudo tee "$USB_TMPFILES" >/dev/null <<'CONF'
# SpectrumJetson: a USB device that stops answering holds up the others on its hub while the
# kernel retries it (~65 s each at the default 5 s). Healthy devices answer in milliseconds.
w /sys/module/usbcore/parameters/initial_descriptor_timeout - - - - 1000
CONF
sudo systemd-tmpfiles --create "$USB_TMPFILES"

echo "==> 11. TCP gives up on a vanished peer after ~13 s, not ~15 min"
sudo tee "$TCP_CONF" >/dev/null <<'CONF'
# SpectrumJetson: retransmissions before TCP gives up on a peer that stopped answering. With data
# queued (a camera stream), the default 15 took ~15 minutes, and a dashboard whose laptop simply
# left kept its streams "watched" and encoded all that time. 5 gives up after ~13 s (RTO 0.2 s
# doubling). NT reconnects by itself; an SSH session dies if the network is out >13 s with data
# waiting.
net.ipv4.tcp_retries2 = 5
CONF
sudo sysctl -q --load "$TCP_CONF"

echo
echo "Summary:"
# (|| true: systemctl is-enabled exits non-zero for disabled/masked units, which is the goal.)
echo "  apt timers: $(systemctl is-enabled apt-daily.timer apt-daily-upgrade.timer 2>&1 | paste -sd' ' || true)"
echo "  snapd: $(systemctl is-enabled snapd.service 2>&1 || true)"
echo "  default target: $(systemctl get-default)"
echo "  jetson-clocks: $(systemctl is-enabled jetson-clocks.service 2>&1 || true), $(systemctl is-active jetson-clocks.service 2>&1 || true)"
for intf in /sys/bus/usb/drivers/uvcvideo/*:1.0; do
  [[ -e $intf ]] || continue
  dev=$(dirname "$(readlink -f "$intf")")
  echo "  camera $(basename "$dev"): power/control=$(cat "$dev/power/control")"
done
echo "  writeback: $(sysctl -n vm.dirty_expire_centisecs vm.dirty_writeback_centisecs | paste -sd/) centisecs (expire/interval)"
echo "  journal: $( [[ -d /var/log/journal ]] && echo "on the SSD (/var/log/journal)" || echo "RAM only")"
fan=$(cat /sys/devices/platform/pwm-fan*/hwmon/hwmon*/pwm1 2>/dev/null | head -1)
echo "  fan: pwm ${fan:-?}/255, nvfancontrol $(systemctl is-active nvfancontrol 2>&1 || true)"
echo "  watchdog: $(systemctl show -p RuntimeWatchdogUSec --value) (rebooting: $(systemctl show -p RebootWatchdogUSec --value)), kernel.panic=$(sysctl -n kernel.panic)"
echo "  USB descriptor timeout: $(cat /sys/module/usbcore/parameters/initial_descriptor_timeout) ms"
echo "  TCP retries before giving up: $(sysctl -n net.ipv4.tcp_retries2) (~13 s at 5)"
echo "  photonvision: Restart=$(systemctl show photonvision -p Restart --value), $(systemctl show photonvision -p Environment --value | tr ' ' '\n' | grep -c OPENCV_THREAD_POOL) OpenCV pool settings"
echo "Reboot to apply the boot changes: sudo reboot"

#!/usr/bin/env bash
# Patched USB camera driver (uvcvideo) that caps how much USB 2.0 bandwidth a camera reserves,
# so 4 Thriftiest Cams fit on the Jetson's four USB-A ports. Run ON THE JETSON.
#
# Why: a UVC camera reserves isochronous bandwidth for what it *asks* for, not what it sends.
# The Thriftiest Cam (1bcf:28c5) asks for the largest alternate setting in every mode
# (3 x 1020 bytes per 125 us microframe, ~196 Mbps) while sending 4-7 MB/s. Every USB 2.0 port on
# the Jetson (the four USB-A ports, the USB-C port, M.2) shares one budget of about 6700 bytes per
# microframe (measured: 6720 fit, 7400 didn't), so only two uncapped cameras fit. Capping the request
# at 1280 bytes makes the driver pick alternate setting 7 (2 x 640 bytes, 10.2 MB/s, ~85 KB per frame
# at 120 fps): four fit, plus a fifth at 1600. scripts/jetson/usb-bandwidth.py shows what's reserved.
#
# The patch (kernel/uvcvideo-payload-cap.patch) adds one module parameter,
#   payload_cap=vid:pid:bytes[,vid:pid:bytes...]
# and changes nothing for cameras not listed. kernel/uvcvideo-urb-packets.patch adds urb_packets
# (packets per isochronous URB; 0 = the stock 32; set to 16, URB_PACKETS overrides). The install
# also sets the stock hwtimestamps option (frames stamped by the camera clock). Built from the exact stock source of the running
# kernel, verified by matching the installed driver's srcversion before patching.
#
# Usage:
#   11-uvcvideo-payload-cap.sh              build only (no sudo)
#   11-uvcvideo-payload-cap.sh --install    build, install, reload the driver (restarts PhotonVision)
#   11-uvcvideo-payload-cap.sh --undo       remove it: back to the stock driver
# Env: CAP (default "1bcf:28c5:1280,32e4:0144:1280,32e4:62f0:1600": the Thriftiest Cam, the "Global
# Shutter Camera" 32e4:0144 with the same alternate settings, and the "USB Camera" 32e4:62f0, whose
# settings go 800 / 1600 / 2400 / 3072). The driver picks the smallest alternate setting at least as
# big as the cap. A camera with no small alternate setting (the Razer Kiyo has only 3 x 1020) can't
# be capped, and another port doesn't help: all USB 2.0 ports share the budget.
# Per-camera caps ("port:bytes", e.g. 1-2.4:944) are set on PhotonVision's Camera Matching page
# (the driver's payload_cap is writable at run time); reinstalling keeps them.
set -euo pipefail
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(uname -r)
BASE=v${KVER%%-*}   # 5.15.199-tegra -> v5.15.199
CAP=${CAP:-1bcf:28c5:1280,32e4:0144:1280,32e4:62f0:1600}
W=$HOME/build/uvcvideo-$KVER
DEST=/lib/modules/$KVER/updates/uvcvideo.ko
CONF=/etc/modprobe.d/90-spectrum-uvcvideo.conf
FILES="Kconfig Makefile uvc_ctrl.c uvc_debugfs.c uvc_driver.c uvc_entity.c uvc_isight.c uvc_metadata.c uvc_queue.c uvc_status.c uvc_v4l2.c uvc_video.c uvcvideo.h"

reload() {
  echo "==> Reloading the camera driver (PhotonVision stops for a few seconds)"
  sudo systemctl stop photonvision
  sudo modprobe -r uvcvideo
  sudo modprobe uvcvideo
  sleep 2
  sudo systemctl start photonvision
  echo "    now loaded: $(modinfo -F filename uvcvideo), payload_cap=$(cat /sys/module/uvcvideo/parameters/payload_cap 2>/dev/null || echo "(stock driver)")"
}

if [[ ${1:-} == --undo ]]; then
  # The stock driver has no urb_packets (or payload_cap); hwtimestamps it has, so that stays.
  sudo rm -f "$DEST" "$CONF" /etc/modprobe.d/92-spectrum-uvcvideo-urb-packets.conf
  sudo depmod -a
  reload
  exit 0
fi

# 1. Stock source for this kernel, from the stable kernel's GitHub mirror (git.kernel.org
#    refuses scripted downloads). Same tags/commits as kernel.org.
mkdir -p "$W/stock"
for f in $FILES; do
  [[ -s $W/stock/$f ]] && continue
  python3 - "$BASE" "$f" "$W/stock/$f" <<'PY'
import sys, urllib.request
tag, f, out = sys.argv[1:4]
url = f"https://raw.githubusercontent.com/gregkh/linux/{tag}/drivers/media/usb/uvc/{f}"
open(out, "wb").write(urllib.request.urlopen(url, timeout=30).read())
PY
done

# 2. The stock build must be byte-identical in source to the installed driver.
make -s -C "/lib/modules/$KVER/build" M="$W/stock" CONFIG_USB_VIDEO_CLASS=m modules
stock_src=$(modinfo -F srcversion "$W/stock/uvcvideo.ko")
# The installed driver: the stock one, even if ours is loaded from updates/.
inst=$(find "/lib/modules/$KVER/kernel" -name 'uvcvideo.ko*' | head -1)
inst_src=$(modinfo -F srcversion "$inst")
if [[ $stock_src != "$inst_src" ]]; then
  echo "Stock $BASE source doesn't match the installed driver ($stock_src vs $inst_src)." >&2
  echo "NVIDIA may have patched uvcvideo in this L4T release; not continuing." >&2
  exit 1
fi
echo "==> Stock source matches the installed driver (srcversion $stock_src)"

# 3. Patch and build.
rm -rf "$W/patched" && cp -r "$W/stock" "$W/patched"
(cd "$W/patched" && make -s -C "/lib/modules/$KVER/build" M="$PWD" clean >/dev/null 2>&1 || true)
patch -s -d "$W/patched" -p5 < "$REPO_ROOT/kernel/uvcvideo-payload-cap.patch"
# urb_packets: packets per isochronous URB (kernel/uvcvideo-urb-packets.patch; 0 = stock 32).
patch -s -d "$W/patched" -p5 < "$REPO_ROOT/kernel/uvcvideo-urb-packets.patch"
make -s -C "/lib/modules/$KVER/build" M="$W/patched" CONFIG_USB_VIDEO_CLASS=m modules
for parm in payload_cap urb_packets; do
  modinfo "$W/patched/uvcvideo.ko" | grep -q "^parm:.*$parm" \
    || { echo "Patched driver has no $parm parameter?" >&2; exit 1; }
done
echo "==> Built $W/patched/uvcvideo.ko (vermagic $(modinfo -F vermagic "$W/patched/uvcvideo.ko"))"

[[ ${1:-} == --install ]] || { echo "Build only. Install with: $0 --install"; exit 0; }

# 4. Install next to the stock driver (updates/ wins), with the cap as a module option. Keep any
#    per-port caps set on the Camera Matching page (entries like 1-2.4:944).
ports=$(sed -n 's/^options uvcvideo payload_cap=//p' "$CONF" 2>/dev/null | tr ',' '\n' | grep -E '^[0-9]+-[0-9.]+:[0-9]+$' | paste -sd, - || true)
[[ -n $ports ]] && CAP="$CAP,$ports" && echo "==> Keeping the per-camera caps: $ports"
sudo install -D -m 644 "$W/patched/uvcvideo.ko" "$DEST"
printf '# SpectrumJetson: cap USB bandwidth reservations (scripts/jetson/11-uvcvideo-payload-cap.sh)\noptions uvcvideo payload_cap=%s\n' "$CAP" \
  | sudo tee "$CONF" >/dev/null
# Frames timestamped with the camera's clock (its UVC PTS, through the SCR) instead of the first
# USB packet's arrival: robot-side jitter 0.95 ms -> 0.01 ms (tests/fake-robot/timestamps.sh). The
# stock driver has this option too; its own file so the Camera Matching page never rewrites it.
printf '# SpectrumJetson: timestamp frames with the camera clock (scripts/jetson/11-uvcvideo-payload-cap.sh)\noptions uvcvideo hwtimestamps=1\n' \
  | sudo tee /etc/modprobe.d/91-spectrum-uvcvideo-timestamps.conf >/dev/null
# 16 packets per isochronous URB instead of 32 (kernel/uvcvideo-urb-packets.patch): the driver sees
# a frame's last packet up to 2 ms sooner. Latency 12.98 -> 11.76 ms at the result, 5 cameras;
# ~0.1 core more (kernel), +2,400 interrupts/s. 8 gained only 0.15 ms more for more CPU.
printf '# SpectrumJetson: 16 packets per URB, 1.2 ms lower latency (scripts/jetson/11-uvcvideo-payload-cap.sh)\noptions uvcvideo urb_packets=%s\n' "${URB_PACKETS:-16}" \
  | sudo tee /etc/modprobe.d/92-spectrum-uvcvideo-urb-packets.conf >/dev/null
sudo depmod -a
reload

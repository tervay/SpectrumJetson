# SpectrumJetson: technical reference

Imaging and setup for a **Jetson Orin Nano Super Developer Kit** (P3768 carrier +
P3767-0005 8GB module) running PhotonVision with FRC 971's CUDA AprilTag detector,
for Spectrum 3847/8515. Boots from NVMe, with no SD card.

Technical reference for the setup. For the student-friendly overview, start with the [README](../README.md). Original brief: [HANDOFF-jetson-flash.md](HANDOFF-jetson-flash.md).
Where this document disagrees with the brief, this document wins (see *Changes from the handoff*).

## Target

| | |
|---|---|
| JetPack | **6.2.3** |
| Jetson Linux (L4T) | **36.5.2**, Ubuntu 22.04, kernel 5.15, CUDA 12 |
| Board config | `jetson-orin-nano-devkit-super` |
| Storage | NVMe (`nvme0n1p1`), QSPI bootloader updated during the flash |
| MAXN SUPER | `sudo nvpmodel -m 2` (0 = 15W, 1 = 25W default, 2 = MAXN_SUPER) |

Settings shared by the scripts live in [config.env](../config.env).

## Flashing (from an Ubuntu 22.04 x86-64 host)

1. **Download** the BSP and sample rootfs into `~/nvidia/r36.5.2/`:
   ```bash
   mkdir -p ~/nvidia/r36.5.2 && cd ~/nvidia/r36.5.2
   B=https://developer.nvidia.com/downloads/embedded/l4t/r36_release_v5.2/releases
   curl -fLO $B/Jetson_Linux_r36.5.2_aarch64.tbz2
   curl -fLO $B/Tegra_Linux_Sample-Root-Filesystem_r36.5.2_aarch64.tbz2
   ```
2. **Prepare the BSP** (extract, host prerequisites, apply binaries, create the default
   user). Prompts for username, hostname and password:
   ```bash
   scripts/host/01-prepare-bsp.sh
   ```
3. **Put the Jetson in Force Recovery Mode:**
   - Leave the USB-C data cable connected from the carrier's USB-C port to the host.
   - Unplug barrel power.
   - Jumper **FC REC** to **GND** (pins 9 and 10) on the **12-pin button header J14**.
     It's under the module on the carrier edge. It is *not* the 40-pin GPIO header.
   - Plug power in, wait about 2 s, and remove the jumper.
   - `lsusb` should now show `0955:7523 NVIDIA Corp. APX`.
4. **Flash:**
   ```bash
   scripts/host/02-flash-nvme.sh
   ```
   This takes about 10-20 minutes and writes a log to `logs/`. It temporarily stops
   NetworkManager from managing the Jetson's USB network interface and opens ufw
   for `fc00:1:1::/48`, then restores both.
5. **Get into the Jetson.** After the flash it boots from NVMe and shows up over the
   USB-C cable as `0955:7020`. The Jetson is `192.168.55.1`; the host gets
   `192.168.55.100` by DHCP. Install a key so later steps can run over SSH:
   ```bash
   ssh-keygen -t ed25519 -N "" -f ~/.ssh/jetson_ed25519   # once per host
   ssh-copy-id -i ~/.ssh/jetson_ed25519.pub spectrum3847@192.168.55.1
   ```
   Copy the scripts over:
   ```bash
   tar czf - --exclude=logs --exclude=.git . | ssh -i ~/.ssh/jetson_ed25519 spectrum3847@192.168.55.1 'mkdir -p ~/SpectrumJetson && tar xzf - -C ~/SpectrumJetson'
   ```
6. **Verify, then enable MAXN SUPER** (on the Jetson):
   ```bash
   ~/SpectrumJetson/scripts/jetson/01-verify.sh
   ```
   Expected: all three PASS lines, and `nvpmodel -q` reports `MAXN_SUPER` / `2`. At
   idle, `tegrastats` shows all 6 CPU cores at 1728 MHz. The mode persists across
   reboots (`/var/lib/nvpmodel/status` = `pmode:0002`); `jetson_clocks` does not.
7. **Get the Jetson online.** It has no internet over USB, and its clock is wrong
   until NTP syncs, which breaks apt's TLS. Wi-Fi is easiest; the password is prompted
   for, not echoed:
   ```bash
   sudo nmcli --ask dev wifi connect <SSID> ifname wlP1p1s0
   ```
8. **Install JetPack components** (CUDA/cuDNN/TensorRT) and the build environment
   (on the Jetson):
   ```bash
   ~/SpectrumJetson/scripts/jetson/02-jetpack.sh
   ```

## Gotchas seen on the first flash (2026-09-23)

- **Recovery-mode header:** the 12-pin J14 is tucked under the module. The 40-pin
  header is the wrong one.
- **Password leak:** NVIDIA's `l4t_create_default_user.sh` prints the password in
  plain text. `01-prepare-bsp.sh` now masks it. Change the password with `passwd`
  after first boot if it was ever shown.
- **"Waiting for target to boot-up..."** repeats for about 30 s while the flashing
  initrd boots. That's normal. The flash is only done at `Flash is successful`,
  once the QSPI write after "Successfully flashed the external device" finishes.
  Don't unplug the board at the external-device message.
- **Harmless flash warnings:** "backup GPT table is corrupt", missing
  `/dev/mmcblk0boot0` (there's no eMMC), "Skip writing ... no image is specified".
- The whole flash took about 7 minutes on a 16-core host.
- **The shop network (`spectrum3847` Wi-Fi) blocks `frcmaven.wpi.edu`.** Fortinet
  FortiGuard DNS filtering resolves it to a block page (`2620:101:9000:53::55`, cert
  `CN = Fortiguard SDNS Blocked Page`), so Java reports a PKIX/SSL error. Gradle builds
  that need WPILib artifacts (the PhotonVision fork, GradleRIO robot code) must run on
  another network, or the domain needs allowlisting. GitHub and
  maven.photonvision.org are not blocked.

## Vision stack: 2026 CUDA fork on the Jetson, 2027 robot code

**Target event:** October 2026 off-season, playing as **team 8515**. Robot code is
[`Spectrum3847/2026-FM-SystemCore`](https://github.com/Spectrum3847/2026-FM-SystemCore)
(WPILib 2027.0.0-alpha-6 on a **SystemCore**, vendordep `photonlib v2027.0.0-alpha-2`).
The field is the **2026 Rebuilt AndyMark** layout, on the Jetson and in robot code.

**Decision:** the Jetson runs the **unmodified 2026** `FRC-Team-4143/photonvision` fork
(`d8c9e8e`, WPILib 2026.2.1) with 971's CUDA detector. The robot uses **stock**
photonlib alpha-2. This works because everything on the wire is identical between the
fork's base and the alpha-6 era (checked in source on 2026-09-23, not yet on hardware):

- **Serde hashes match** (`PhotonPipelineResult` = `4b2ff16a964b5e2bf04be0c1454d91c4`,
  and all sub-messages). PhotonLib only throws on a hash mismatch; a different
  version string just logs.
- **NT4:** the same subprotocol, port 5810 and encoding. 2026 and 2027 clients both
  try `10.TE.AM.2` first, so SystemCore is `10.85.15.2`.
- **Time sync:** the same UDP 5810 packet layout and microsecond timebase.

> **Do not upgrade the robot's photonlib past the alpha-6 era.** PhotonVision `main`
> after alpha-7 (`a6167b0`, 2026-09-17) renamed the timestamp fields, which changed the
> hashes, and the robot would throw against this Jetson.

Porting CUDA to 2027 PhotonVision was rejected. 2027 allwpilib needs JDK 25, C++23
and GCC 13 (Ubuntu 24.04), while JetPack 6 has GCC 11.

| Piece | Version | Built on | Script |
|---|---|---|---|
| allwpilib | `v2026.2.1` (not `main`: `wpi/jni_util.h` moved) | Jetson, `-j4` (more OOMs on wpimath); **17 min** in MAXN SUPER | `scripts/jetson/04-build-allwpilib.sh` |
| GpuDetectorJNI | `FRC-Team-4143` `ef9fc1e`, CUDA arch 87 → `/usr/lib/lib971apriltag.so` | Jetson | `scripts/jetson/05-build-gpudetector.sh` |
| PhotonVision fork jar | `d8c9e8e`, Java 17 target | Laptop (Node 22, pnpm 10, Temurin 17, as in CI) | `scripts/host/03-build-photonvision-fork.sh` |
| Java runtime | **17** for the fork (the PV 2027 installer made 25 the default) | systemd drop-in `photonvision.service.d/java17.conf` | `scripts/jetson/06-install-fork-jar.sh <jar>` |

`scripts/jetson/03-photonvision.sh` installs upstream `v2027.0.0-alpha-2` (CPU only).
That's a placeholder, and it provides the systemd service; the fork jar replaces its
jar.

Known detector rough edges: per-detection `std::cout` in the hot path, and a maximum
of 10 detector handles per process, never recycled. The native lib casts a `jlong` to
`cv::Mat*` compiled against JetPack's OpenCV 4.8 headers while PhotonVision runs its
bundled OpenCV 4.10. That's fine while `cv::Mat`'s layout is unchanged, but fragile.

## First CUDA results (2026-09-23)

One Thriftiest Cam, 1280×800 MJPEG, AprilTagCuda pipeline. Timing comes from
`gpudetector-02-timing-stats.patch` (`971 stats …` lines in `journalctl -u photonvision`).

| Stage | Cost | Limit |
|---|---|---|
| Camera, 1280×800 MJPEG (measured with `v4l2-ctl --stream-mmap`, PV stopped) | n/a | ~120 fps |
| **Exposure.** The UI's "µs" is really **100 µs units** (V4L2 `exposure_time_absolute`); 295 = 29.5 ms | n/a | ~34 fps at 295; ~50 fps at 100; ~61–63 fps at ≤83 |
| PhotonVision capture (MJPEG decode → BGR → gray, streams) | ~16 ms/frame | **~63 fps (current bottleneck)** |
| 971 CUDA detector | **1.8–3.5 ms/frame** | 300+ fps |

- **Exposure under shop lights:** mains lighting flickers at 120 Hz (8.33 ms). Under
  about 70 (7 ms), detections looked unstable in the UI, yet the raw detector found the
  tag in **100% of frames** at every exposure from 30 to 295. The flicker comes from
  PhotonVision's decision-margin filter (default 35), not from detection. Use **~83**
  in the shop, and lower the decision margin if needed. Retune on the event field.
- **The camera has no UVC gain control**, only exposure and brightness.
- Other teams on Chief Delphi report the same ~32–36 fps with an idle GPU (thread
  483803). 4143 reported 2×1280×800 at 55 fps each.

- **3D mode needs a calibration at the active resolution** (ChArUco, in the Calibration
  tab). The intrinsics also feed the 971 detector via `setparams`.
- **Stray CUDA error** (a known CCCL 2.5 behavior, [NVIDIA/cccl#1791](https://github.com/NVIDIA/cccl/issues/1791)).
  After switching pipeline type and resolution while running,
  every frame logged `Check failed: cub::DeviceSelect::If(...) (invalid device
  ordinal)`. CUB checks `cudaPeekAtLastError()`, so a *stale* error from an unchecked
  call (e.g. the unchecked `cub::DeviceReduce::ReduceByKey`) makes the peak-filter
  select bail out and use stale data, while detections still appear.
  [`patches/gpudetector-01-cuda-peek.patch`](../patches/gpudetector-01-cuda-peek.patch) clears
  pending errors after each stage and logs the first one per stage as
  `CUDA_PEEK after <stage>`. After a clean restart the error hasn't come back, so
  the root cause is still unconfirmed. If it reappears, the log names the stage.
- Streams render in Firefox; the in-app browser pane doesn't show them.
- The camera is on the devkit's single onboard USB 2.0 hub (all 4 USB-A ports), so
  two cameras will share 480 Mbps.

## Detector source: where the current 971 code lives (research, 2026-09-23)

- `frc971/971-Robot-Code` is **archived**. Austin Schuh's live code is in
  **RealtimeRoboticsGroup/aos `frc/orin/`** (HEAD `8d8a7315e`), used by 4646/1868.
  971's own CMake/nvcc build of it for CUDA 12.6 / sm_87 is
  **frc971/bos `third_party/971apriltag`**, and frc971/cos adds CUDA 13 shims.
- The 4143 copy matches upstream from about 2024-08-11. It is missing, among others,
  **`3e570d5a` (a memory leak on every quad decode)**, `86f0ac3f` (32-bit types),
  `8e7d6743` (async memcpy) and the `76d8f216` tuning.
- **4143 JNI bugs:** detector slots are never reused, so after 10 creates it uses
  `detectors[-1]` (UB), which pipeline switches and resolution changes can trigger;
  `delete` is used on an `apriltag_detector_t`; the tag family leaks; `CHECK_CUDA`
  only prints.
- `mashed26/GpuDetectorJNI` has a better JNI layer (handle map, proper destroy,
  CCCL 3), but a different Java API, so it isn't a drop-in replacement.

## Detector builds: which lib971apriltag.so is which

Both expose the same Java API, so the PhotonVision fork jar works with either.
Swap by installing one to `/usr/lib/lib971apriltag.so` and restarting `photonvision`.

| Build | Source | Script | Status |
|---|---|---|---|
| **4143 + patches** (fallback) | FRC-Team-4143/GpuDetectorJNI `ef9fc1e` (≈971 code of 2024-08) + `patches/gpudetector-0{1,2,3}` | `05-build-gpudetector.sh` (installs) | Running; leak, handle and stale-error fixes applied |
| **bos / Austin's current** (**installed**, robot config) | frc971/bos `62e93b4` `third_party/971apriltag` = RealtimeRoboticsGroup/aos `frc/orin` detector as of `8736ba62` (2026-03-30) + 971's `absl::Status` returns + `patches/bos-01` to `bos-04`; JNI in `detector/` | `07-build-bos-detector.sh` then `08-select-detector.sh bos --mwbd 20` | A/B tested and fault tested (below) |

aos is the upstream source of truth. The only detector change in aos since bos
imported it (2026-04-03) is `c1c3b4607` (M_PI → std::numbers::pi, cosmetic).

### A/B results (2026-09-23, one camera, 1280×800 MJPEG, tag held still)

`tests/detector-ab/run.sh` (stats averaged over 6 s per row):

| Build | min_white_black_diff | Exposure | FPS | Detect | Tags/frame | Decision margin |
|---|---|---|---|---|---|---|
| 4143 + patches | 5 | 30 | 62.8 | 2.40 ms | 1.00 | n/a |
| 4143 + patches | 5 | 83 | 61.1 | 3.01 ms | 1.00 | n/a |
| bos | 5 | 30 | 62.8 | 2.39 ms | 1.00 | 43.9 |
| bos | 5 | 83 | 61.1 | 3.03 ms | 1.00 | 118.5 |
| **bos** | **20** | 30 | 62.8 | **1.71 ms** | 1.00 | 43.8 |
| **bos** | **20** | 83 | 61.2 | **1.79 ms** | 1.00 | 118.5 |

- **Selected for the robot: bos, `min_white_black_diff` 20**
  (`08-select-detector.sh bos --mwbd 20`). The detector is 30–40% faster, with
  identical detection and margins. FPS is capture-bound either way.
- **Decision margin tracks exposure:** about 44 at 3 ms vs about 118 at 8.3 ms. With
  PhotonVision's default cutoff of 35, short exposures under shop lights sit close to
  the cutoff, which is why tags flickered. Pick exposure and cutoff together.

### Two cameras, Low Latency Mode (2026-09-23)

- **Low Latency Mode off** (PhotonVision's non-blocking capture) with one camera:
  61 → **~100 fps** at the same ~18 ms latency, but Java CPU 113% → 191%. With it
  on, the capture loop waited for each frame and missed every other one.
- **Two Thriftiest Cams**, both 1280×800 MJPEG, exposure 83, AprilTagCuda, bos
  mwbd 20, Low Latency off: **~92 fps each, ~20 ms latency**, 1.00 tags/frame,
  margins ~122, 0 errors. Java uses ~2.8 of 6 cores (one core ~98% on MJPEG decode),
  GPU 18%, 55 °C, 10.3 W. The shared USB 2.0 hub handles both at full resolution.
- **The cameras are indistinguishable to software:** same name (`Thrifty:`) and serial
  (`01.00.00`). PhotonVision tells them apart by USB port: `Thrifty:_` is on port 2.1
  (`/dev/video0`), `Thrifty:_ (1)` on port 2.3 (`/dev/video2`). **Keep each camera in
  its port**, or calibrations and robot-to-camera transforms swap.

### Calibration board (2026-09-23)

The team's board is a ChArUco, DICT_5X5, 30 mm squares, 22 mm markers. It's labeled
"9x12", but in PhotonVision it must be entered as **Board Width 12, Board Height 9**.
9×12 recovers 0 corners and makes mrcal fail with "Negative corner in reprojection
error calc" or null intrinsics. Use Tag Family `Dict_5X5_1000`, Pattern Spacing
**1.181 in**, Marker Size **0.866 in** (this PV version takes inches), and Old OpenCV
Pattern **off** (9 rows is odd, so both layouts are identical). Verified with
`tests/charuco-board-check/check_board.py` on a live frame: 78/88 corners, 51/54 markers.

Calibration uses its own camera settings. It switched to auto exposure at 20, which
gave a near-black image. Set Auto Exposure off and Exposure ~150 in the calibration card.

### Lens distortion: all 8 coefficients (2026-09-24)

PhotonVision's calibration (mrcal) produces the 8-coefficient OpenCV rational model
(`k1 k2 p1 p2 k3 k4 k5 k6`). The 4143 fork passed only the first 5 to the CUDA
detector, which uses the model to undistort tag edges during corner refinement and then
re-distort the corners. `photonvision-04-dist-coeffs-8.patch` adds `setparams8`
(implemented in `detector/GpuDetectorJNI.cc`), and the log now shows
`setparams handle N (8 dist coeffs)`. It falls back to 5 if the 4143 library is installed.

Bench calibrations (`tests/calibration-check/check_calibration.py`): camera on port 2.1:
43 snapshots, 97% of corners kept, mean 0.87 px, fx 737.8, cx/cy 650.5/362.4. Camera on
port 2.3: 41 snapshots, 96% kept, mean 0.97 px, fx 737.0, cx/cy 597.9/371.6. Handheld
calibrations wouldn't go below ~0.8 px. The outlier rate is the useful quality signal:
42% when the board hung off the frame, 3–4% when it stayed inside and touched the edges.

### Robot tuning and boot time (2026-09-24)

`scripts/jetson/09-robot-tuning.sh` (`--undo` reverses it):
- apt timers disabled, plus an APT::Periodic override
- snapd masked (no snaps installed; `snapd.seeded` took 45 s of every boot)
- `multi-user.target` default (headless)
- `jetson-clocks.service` (After=nvpmodel, Before=photonvision)
- a udev rule setting `power/control=on` for every uvcvideo device

Measured after reboot:

| | Before | After |
|---|---|---|
| `systemd-analyze` | 56.9 s (6.9 kernel + 50.0 userspace) | **16.5 s** (9.0 + 7.6) |
| PhotonVision started (s since kernel start) | ~12 (it never waited on snapd) | 14.7 (now after jetson_clocks) |
| First detection, cameras 1 / 2 | not measured | **19.9 / 20.1 s** |

Everything survived the reboot: MAXN SUPER, clocks locked (CPU 1728 MHz, GPU 1020 MHz),
camera autosuspend off, the bos detector (mwbd 20), and both calibrations (8
coefficients). `health-check.sh` reports READY; the only warnings are no robot and Wi-Fi on.

JVM (`tests/jvm-check.sh`, 2 cameras at ~90 fps): 28 MB peak heap of 512 MB, 0 GCs in
20 s, 787 MB RSS (native frame memory). `-Xmx512m` stays.

### Blank frames and the watchdog (2026-09-24)

- **Bug:** with zero candidate blobs, `num_selected_blobs_host == 0`, and `FitLines` computed
  `kBlocks = 0` and launched `<<<0, 128>>>`. That invalid configuration surfaced in the
  peak-filter `cub::DeviceSelect::If` (apriltag.cc:1013) as status 101 on every frame.
  `bos-02-empty-frame.patch` returns no detections early and guards `FitLines`.
  `tests/detector-frame-sizes/run.sh`: blank frames at 1280x800, 640x480, 320x240, 800x600
  and 1280x720 all failed before; all pass after. Noise frames always passed.
- **Watchdog:** on failure, the JNI calls `cudaDeviceSynchronize()`. It exits (for a systemd
  restart) only if the context stays broken for 1 s. Fault tests: 1 error per 100 frames →
  no restart; an error on every frame → **no restart**, 94 fps with 0 errors once it stops;
  sticky null-pointer kernel fault → exit after 1.3 s, detecting again 6.5 s later.
- **Team defaults:** `photonvision-06` now picks the resolution once the camera reports its
  video modes (they're empty when the pipeline is first created). Verified: "team default
  resolution 1280x800 kMJPEG @ 120 fps (mode 13)".
- **3D and multi-tag follow the calibration:** new cameras get `doMultiTarget = true`. When a
  calibration is saved or imported (`addCalibrationToConfig`), or a new camera's resolution
  is picked, every AprilTag/AprilTagCuda pipeline whose resolution now has a calibration gets
  `solvePNPEnabled = true` and multi-tag on. Never turns either off. (3D stays off without a
  calibration because the pose pipes have no intrinsics then.) Verified: TopLeft switched to
  2D, same calibration re-imported through `/api/calibration/importFromData` → "turned on 3D
  and multi-tag for pipeline "New Pipeline"", 97–103 fps after. Found because TopLeft had
  lost 3D, and both cameras had multi-tag off, after TopLeft was re-created as BottomLeft.

### Rewind recording (2026-09-24)

`patches/photonvision-07-rewind.patch`; the full description is in [REWIND.md](REWIND.md).

- **How it records.** It adds a per-camera `RewindRecorder` in `USBFrameProvider`: a second cscore `RawSink` on the `UsbCamera`, left at `kUnknown` pixel format, so `GetExistingImage(0)` hands over the camera's own MJPEG bytes. No decode, no re-encode.
- **Threads.** One thread per camera, at nice 10 via `renice` on `/proc/thread-self`. It keeps a frame if ≥ 1/fps − 2 ms has passed since the last kept frame (30 fps).
- **Control.** `RewindManager` runs a 5 Hz tick: robot NT `record` (plus a 60 s grace period if the robot disconnects) or the UI's bench switch. It also enforces the quota and minimum free space. Files go to `/opt/photonvision/rewind`.
- **Measured** (2 cameras, `tests/rewind-ab/run.sh 30 2`): detector fps 108–110 / 99–100 off, and the same with recording on; detect time 1.5–1.9 ms either way. Rewind threads use 2.9% of one core. Frames are 30–55 KB; TopRight's view compresses better.
- **WPILib bug:** `RawFrame.getSize()` returns the limit of a Java `ByteBuffer` that is only replaced when the native data pointer changes. `WPI_AllocateRawFrameData` frees and mallocs, which often returns the same address, and it doesn't reallocate at all when the frame fits the capacity. So the limit stuck at the first frame's size: every frame was 51,677 bytes, with no EOI marker. Fix: `RawFrame.setData()` with our own 4 MB direct buffer (the JNI gives it a no-op free), and the real length from the JPEG (walk the marker segments, then the first `FFD9` in the scan data). Verified: 1,005/1,005 frames complete, sizes 54.3–54.8 KB, the AVI decodes end to end in GStreamer.
- **Export** (`scripts/host/rewind-export.py`). A plain-Python MJPEG AVI writer (RIFF `hdrl`/`movi`/`idx1`, split under 2 GB), plus `frames.csv` with `jetson_us` and `robot_us`. Optional H.264 `.mp4` via ffmpeg's concat demuxer with per-frame durations.
- **Download** (`GET /api/rewind/download?session=NAME`, the button in the Rewind card). `RewindExport` streams a zip: stored entries (deflate level 0), with each AVI's layout computed from the CSV index first so it's written in one pass. The handler thread runs at nice 10 while sending. Its AVI is byte-for-byte identical to `rewind-export.py`'s (checked on a 1,005-frame recording). The response has no `Content-Encoding`, so Javalin doesn't gzip it. Speed: 70 MB/s single, 125 MB/s back to back over USB. Cost while downloading: detector fps 93 → 78 (TopLeft) and 105 → 92 (TopRight), from kernel network/softirq time that nice doesn't cover. Not throttled: downloads only happen with the robot disabled.
- **Not yet verified:** `robot_us` (Jetson time + `TimeSyncManager.getOffset()`) needs the robot network.

### Power-cut safety (2026-09-24)

The robot is switched off, never shut down, so every power-off is a power cut.

- **Filesystem.** ext4 with its journal (default `data=ordered`, barriers on), and the NVMe's volatile write cache honors flushes. A cut leaves the filesystem consistent; the kernel replays the journal at the next mount.
- **No boot-time fsck.** The L4T initrd mounts root read-write itself (`init` `_mount_root`), so `systemd-fsck-root` is skipped, and the initrd has no `e2fsck`. Adding one means modifying NVIDIA's initrd, which an L4T update would overwrite, so it isn't done. Instead, `health-check.sh` FAILs if `/sys/fs/ext4/<dev>/errors_count` is non-zero. The repair is restoring the backup image.
- **Writeback.** Changed from 30 s / 5 s to `vm.dirty_expire_centisecs=300` and `vm.dirty_writeback_centisecs=100` (`09-robot-tuning.sh` step 6): written data reaches the SSD within ~3 s.
- **System log.** It was RAM-only (`/var/log/journal` didn't exist), so every cut erased it, including the log of a brownout. Now `Storage=persistent`, `SyncIntervalSec=5s`, `SystemMaxUse=300M`.
- **Rewind.** The video file, then the index, are forced to the SSD every 2 s and on close; `session.json` is synced too. Cost: ~0.7% of the recording's frames come late (5 of 704) when a sync blocks the recorder thread. Detector fps is unchanged.
- **Test.** `tests/power-cut/run.sh` (laptop): records, you pull the plug, then after boot it checks the ext4 errors and journal replay, the log from before the cut, PhotonVision's health, and the seconds of video lost.
- **Clock.** No RTC battery on the devkit: after a cut the clock restarts at **1970** until NTP (Wi-Fi) corrects it. That confuses `journalctl -b -1` (use `_BOOT_ID=`), and Rewind names made before NTP carry a 1970 date; their leading number is what orders them.
- **Result (2026-09-24, power pulled 21.4 s into a bench recording):** 0 ext4 errors; the kernel logged `1 orphan inode deleted` / `recovery complete` (the journal replayed, normal after a cut). The old boot's log survived up to 3 s before the cut, including PhotonVision's lines. PhotonVision came back healthy (90 / 105 fps, both calibrations with 8 coefficients). The recording kept 20.0 s of 21.4 s: **1.4 s lost**, every saved frame a complete JPEG. `session.json` has no end, as expected.

### Jetson clock from the robot (2026-09-24)

`patches/photonvision-08-robot-clock.patch` (`RobotClockSync`, 1 Hz).

- **Where the time comes from.** Robot code publishes `/photonvision/clock/unixMs` (integer, `System.currentTimeMillis()`) once the Driver Station has set the robot's clock. The Jetson has no RTC battery and no internet at events, so this is its only source.
- **When it sets the clock.** When the value is 2026–2100, less than 5 s old (NT local receive timestamp) and more than 1 s off. At most once per 30 s, with `date -u -s @…` (PhotonVision runs as root).
- **Internet time wins.** Skipped if `/run/systemd/timesync/synchronized` exists, i.e. timesyncd got NTP time this boot (shop Wi-Fi). The first bench test showed why: timesyncd noticed the jump and put NTP time back within a second, so a wrong robot clock and NTP would fight every 30 s. At events there's no NTP, so the robot's clock is used.
- **Bench test (fake robot 120 s fast, before the NTP rule):** `Clock set from the robot: was 04:57:20, now 04:59:20 (+120.0 s)`, with that log line itself stamped 04:59:20, so the clock really moved. timesyncd then restored NTP time.
- **Next boot.** It then touches `/var/lib/systemd/timesync/clock`; timesyncd moves the clock up to that file's modification time at boot, so after a power cut the Jetson starts near the last robot time, not 1970.
- **Not for vision.** Frame timestamps, the PhotonVision↔robot time sync and Rewind frame times use `nt::Now` (monotonic), which setting the date doesn't move. It fixes Rewind names, the system log, and file dates.
- **Robot half:** [2026-FM-SystemCore#10](https://github.com/Spectrum3847/2026-FM-SystemCore/issues/10), section 6.
- **Test:** `tests/robot-clock/run.sh` (a fake robot NT server on the Jetson).

### Match readiness: fan, watchdog, camera unplug, backups (2026-09-24)

- **Fan** (changed 2026-09-25: the default is NVIDIA's `quiet` profile again, at Allen's request. `09-robot-tuning.sh` runs `jetson_clocks` without `--fan` and keeps nvfancontrol running: 775 rpm, PWM 41, at 43 °C on the bench. `FAN=full 09-robot-tuning.sh` restores full speed, described below.) NVIDIA's `quiet` profile ran the fan at ~2,000 rpm at 56 °C. The profile tables in `/etc/nvfancontrol.conf` are inverted (PWM 255 = off) and it's hard to tell which profile cools harder at a given temperature, so instead `jetson-clocks.service` runs `jetson_clocks --fan`: it stops nvfancontrol and sets `pwm1=255`. It's ordered `After=nvfancontrol.service`, so nvfancontrol can't take the fan back at boot. After a reboot: pwm 255, 5,586 rpm, hottest sensor **56 → 43 °C** (2 cameras, bench). `health-check.sh` reads the real speed from the tachometer (the `pwm_tach` hwmon), not just `pwm1`, which is only what the fan was told: FAIL under 1,000 rpm while the fan is told to spin (unplugged, jammed or dead), WARN under 4,500 rpm at full speed (5,586–6,327 rpm seen), WARN if nvfancontrol is running or `pwm1` isn't full.
- **Hangs.**
  - `RuntimeWatchdogSec=30s` in `/etc/systemd/system.conf.d/zz-spectrum-watchdog.conf`. NVIDIA's own `watchdog.conf` sets 120; systemd reads the files in name order and the last one wins, so ours is named `zz-`.
  - `kernel.panic=3`. NVIDIA already sets `panic_on_oops=1`, but the default `panic=0` means a panic hung until the watchdog fired.
  - PhotonVision `Restart=always`, `StartLimitIntervalSec=0`.
  - All four checked after a reboot.
- **Camera unplug** (`tests/camera-replug/run.sh`, TopLeft pulled for 7.4 s):
  - cscore saw the disconnect at once and retried every ~0.3 s; TopRight kept detecting.
  - After re-plugging: reconnected at 1280x800 in 0.35 s, detecting again on the same detector handle (calibration kept) in 0.9 s, full 105 fps within 2 s.
  - This kernel logs a re-plug as `new high-speed USB device number N`, not `New USB device found`.
- **Backups.**
  - `scripts/host/04-backup-ssd.sh` wraps NVIDIA's `tools/backup_restore/l4t_backup_restore.sh -e nvme0n1 -b`: the Jetson boots a small system over the USB-C cable in recovery mode and NFS-mounts `tools/backup_restore`. The APP partition is saved as a `tar.zst` of its files, the rest with `dd`. The script also saves PhotonVision's settings export, `jetson-info.txt` and `SHA256SUMS`, and refuses to run with more than 1 GB of Rewind recordings on the SSD.
  - First backup (2026-09-24): 21 GB used on the SSD → 8.7 GB backup, about 7 minutes. Afterwards the Jetson stays in NVIDIA's backup system (USB `0955:7035`) until it's power-cycled.
  - `05-restore-ssd.sh` checks the checksums and asks you to type `restore`. It *moves* the backup into `tools/backup_restore/images` for the restore (a symlink wouldn't resolve over NFS on the Jetson) and moves it back afterwards.
  - Both need the same host tweaks as flashing (NetworkManager, ufw) plus udisks2 stopped.
  - A restore can target a blank spare SSD. The QSPI bootloader isn't in the backup, so a replacement *module* needs `02-flash-nvme.sh` first. **Not run yet.**

### Fanless: stock heatsink with the fan off (2026-09-24)

Question: can a sealed, fanless Jetson survive matches? Both tests used the stock devkit heatsink on the bench with `pwm1=0` (0 rpm), MAXN SUPER, clocks locked, each stopped by a safety cutoff. Throttling starts at **99 °C** (CPU/GPU `passive` trips), shutdown at 104.5 °C. The 70 °C trip is only `hot-surface-alert`.

- **Power** (`VDD_IN`): 8.6–9.5 W with 2 cameras detecting, 6.8 W with PhotonVision stopped, 5.7 W with the clocks unlocked too (all at ~40 °C; each rises ~0.5 W by 80 °C). Idle is 60–70% of full power, so throttling while disabled helps less than you'd expect.
- **Full power, fan off:** 42 → 85 °C in 9.7 min, still rising 2.3 °C/min. No throttling.
- **Match cycle, fan off:** 15 min with PhotonVision stopped (7.2 W), then full power: 40 → 63.5 °C at 5 min, 73.7 at 10, 80.5 at 15, then **88 °C after 2 min 39 s** of full power (cutoff).
- **Model** (one RC node fitted to both runs, within 0.8 °C): 6.2 °C/W, time constant 6.6 min, steady state **99 °C at full power**, 85 °C at 7.2 W. Peak at the end of a 4-minute full-power match after being on disabled at 7.2 W: 67 °C (0 min), 80 (5 min), 86 (10 min), ~90 (20+ min). It ran ~1.5 °C under the measured peak.
- **Takeaway:** the stock heatsink alone isn't enough; the duty cycle only helps if the robot isn't on long before the match. A fanless design needs roughly **4 °C/W or better** (by the same model: ≤ 73 °C worst case), and should be tested with this match cycle in its real enclosure. A dead fan isn't fatal: the Jetson slowly climbs to its throttle point instead of shutting down.
- **Throttling while disabled** already works from robot code: `PhotonCamera.setEnabled(false)` (patch 11) or a robot-set FPS limit. If Rewind is used, keep full rate until its 10 s tail ends.

### Decode speedup (2026-09-24)

Full write-up in [VISION-RESEARCH.md](VISION-RESEARCH.md).

- **Profile.** `tests/cpu-profile.sh` (per-thread CPU plus 40 jstack samples):
  - each camera's VisionRunner thread was running 98% of the time, 85% of samples in `CscoreExtras.grabRawSinkFrameTimeoutLastTime`;
  - 5 native threads inherited the name (OpenCV's pthreads pool) at ~22% each.
- **Cause.**
  - cscore 2026.2.1 `Frame::ConvertImpl` turns MJPEG into BGR first (`ConvertMJPEGToBGR`, then `ConvertBGRToGray`) for any requested format. `ConvertMJPEGToGray` (Frame.cpp:406) is never called.
  - Measured on recorded frames: JPEG→BGR→gray 8.9 ms against libjpeg-turbo gray-only 2.6 ms (2.2 ms with the fast IDCT; we keep the accurate one).
- **Fix (`photonvision-09` plus `decodeMjpegGray` in `detector/GpuDetectorJNI.cc`).**
  - PhotonVision grabs through the gray sink with `setInfo(0,0,0,kUnknown)`, so it gets the camera's MJPEG image untouched.
  - The detector library decodes it with libjpeg-turbo (`JCS_GRAYSCALE`, `JDCT_ISLOW`, a longjmp error handler) into a CV_8UC1 Mat that Java allocated.
  - Gotcha: `CscoreExtras.grabRawSinkFrameTimeoutLastTime` fills only the *native* `WPI_RawFrame`; the Java `RawFrame`'s format, size and data stay unset. The first attempt read those, dropped every frame, and broke detection until it was fixed.
  - 30 failures in a row put that camera back on cscore's conversion.
- **OpenCV pool.** `09-robot-tuning.sh` step 9 sets `OPENCV_THREAD_POOL_ACTIVE_WAIT_WORKER=0` and `..._MAIN=0` (the bundled `libopencv_core.so.4.10` reads both).
- **Results** (2 cameras, stream closed, `tests/perf-snapshot.sh`):

  | Stage | fps (TopLeft / TopRight) | CPU |
  |---|---|---|
  | Before | 92 / 104 | 333% |
  | Decode fix | 121 / 121 | ~195% |
  | + OpenCV pool | 122 / 122, 1.6 ms detect | **129%** |

  UI latency went from ~23 ms to **13 ms**.
- **GPU load, 2 cameras at 122 fps** (`tegrastats` every 0.5 s for 30 s, GPU locked at 1020 MHz, capped camera driver, measured after the backup reboot): mean 12%, median 14%, p90 22%, max 24%. Detect 1.95 / 2.16 ms, PhotonVision CPU 145% with one stream open.
- **GPU load, 4 cameras** (2026-09-26: TopLeft/TopRight Thriftiest Cams at 122 fps 1280x800, 2 global-shutter cameras at 61 fps 1280x720; `tegrastats` every 0.5 s for 2 min, 271 samples; GPU locked at 1020 MHz; hardware JPEG decode on; no tags in view, no dashboard streams open). Raw files in `logs/gpu-4cam-20260926/` on the laptop (git-ignored).

  | | mean | median | p90 | max |
  |---|---|---|---|---|
  | GPU (GR3D) | 16.7% | 16% | 19% | 21% |
  | NVJPG / NVJPG1 (JPEG decoders) | 37% / 36% | 37% / 36% | 42% / 41% | 47% / 45% |
  | Memory bandwidth (EMC) | 11% | 11% | 11% | 12% |
  | CPU, all 6 cores | 0.8 cores | 0.9 | 1.3 | 1.9 |
  | Board power (VDD_IN) | 9.6 W | 9.6 | 9.7 | 9.8 |

  PhotonVision itself used 0.73 cores. Detect time avg / worst: 0.98 / 2.03 and 0.95 / 3.06 ms (Thriftiest Cams), 1.21 / 2.25 and 0.85 / 1.87 ms (global-shutter). GPU and tj at 55.6 °C (fan on NVIDIA's quiet profile). About 368 frames/s in total cost ~17% GPU against ~12% for 2 cameras (244 frames/s), so the GPU has plenty of headroom; the two NVJPG engines, at ~37% each, are the busiest hardware. With tags in view the detector does more work per frame (quad fitting, refinement), so expect somewhat more GPU than this empty-scene figure.
- **CUDA wait mode** (`SPECTRUM_971_CUDA_SYNC` or `/tmp/spectrum-971-cuda-sync`), measured with the decode fix:

  | Mode | CPU | Detect time |
  |---|---|---|
  | spin | ~192% | 1.6 / 2.1 ms |
  | block | ~199% | 1.9 / 2.5 ms |
  | yield | ~200% | 2.2 / 2.5 ms |

  With 2 cameras the default stayed `auto` (CUDA's own, spins).
- **4 cameras: CUDA wait and GPU work queues** (2026-09-24, 2 Thriftiest Cams at 122 fps and 2 global-shutter cameras at 61 fps, no tags in view, no dashboard streams, hardware decode on). Each config ran 1 minute after a restart; detect time in ms as average / typical worst (the average of each second's max) / worst. First run:

  | Config | Thriftiest Cams | Global-shutter cameras |
  |---|---|---|
  | baseline (`auto`, 8 queues, 6 threads) | 3.09 / 6.65 / 11.98 | 3.52 / 6.16 / 11.66 |
  | `block` | 2.99 / 5.99 / 11.90 | 2.95 / 5.00 / 8.29 |
  | `CUDA_DEVICE_MAX_CONNECTIONS=32` | 2.81 / 4.87 / 9.34 | 2.81 / 4.60 / 7.15 |
  | 2 detector threads | 3.25 / 6.39 / 9.72 | 3.55 / 5.83 / 8.74 |
  | 3 detector threads | 4.02 / 7.29 / 11.41 | 4.12 / 6.88 / 13.02 |
  | baseline again | 2.73 / 5.95 / 20.17 | 3.62 / 6.39 / 10.04 |

  Second run, back to back (the "32 queues" config above looked like a clear win, so it was repeated alongside block with 32):

  | Config | Thriftiest Cams | Global-shutter cameras | PhotonVision CPU |
  |---|---|---|---|
  | `block` + 32 queues | 2.75 / 5.37 / 10.69 | 3.06 / 5.05 / 8.82 | 1.43 cores |
  | 32 queues (`auto`) | 4.05 / 6.59 / 8.87 | 3.35 / 5.34 / 9.54 | 1.63 |
  | baseline (`auto`, 8 queues) | 2.85 / 6.46 / 9.96 | 3.43 / 6.24 / 10.17 | 1.48 |
  | `block`, 8 queues | 2.88 / 6.32 / 9.26 | 4.17 / 6.31 / 9.89 | 1.50 |
  | `block` + 32 queues again | 3.95 / 7.54 / 11.56 | 4.48 / 7.18 / 11.03 | 1.64 |

  **Neither setting has a proven effect.** The same config varies by up to 2 ms in the typical worst between restarts (block + 32: 5.37, then 7.54), more than the configs differ from each other. Averaging each group's typical worst over all 11 runs:

  | | Runs | Thriftiest Cams | Global-shutter cameras |
  |---|---|---|---|
  | 32 queues | 4 | 6.09 | 5.54 |
  | 8 queues | 7 | 6.44 | 6.12 |
  | `block` | 4 | 6.31 | 5.89 |
  | `auto` | 7 | 6.31 | 5.92 |

  32 queues may be worth ~0.5 ms (CUDA funnels every stream into 8 hardware queues by default, so one camera's kernels can wait behind another's even with the GPU only ~31% busy); `block` makes no difference. GPU ~31% in every run, and CPU tracked the detect times rather than the settings. **Both are the defaults because neither hurts:** `block` (the library's default) and 32 queues (`08-select-detector.sh --gpu-connections`, default 32, written to `971.conf`). The load line shows both ("CUDA wait block, GPU connections 32"), and `health-check.sh` warns if they differ. Proving an effect this small would take many alternating restarts (both settings are read only when CUDA starts). Detector threads (`SPECTRUM_971_THREADS`, or `/tmp/spectrum-971-threads`, applied live within 2 s; default 6) made no clear difference either.
  - **Dashboard streams count.** With 4 streams open in a browser, the same settings ran at ~4 ms average and ~7.7 ms typical worst, and PhotonVision used ~2.0 cores instead of ~1.45. Close the dashboard before measuring.
- **Detector threads with a tag in view** (2026-09-24, block + 32 queues, one hand-held tag seen by both Thriftiest Cams on every frame, 4 dashboard streams open). The thread count was switched live, cycling 6/2/3/4/1 threads; each segment was 3 s to settle and 7 s measured, over 3 rounds:

  | Threads | Tag cameras (ms) | No-tag cameras (ms) | PhotonVision CPU |
  |---|---|---|---|
  | 6 | 4.22 / 7.75 / 10.68 | 3.75 / 7.30 / 13.09 | 2.02 cores |
  | 4 | 4.06 / 6.99 / 8.82 | 3.60 / 6.77 / 8.85 | 1.93 |
  | 3 | 4.17 / 7.38 / 9.80 | 3.68 / 6.88 / 9.23 | 1.95 |
  | 2 | 4.19 / 7.43 / 9.41 | 3.71 / 6.98 / 9.39 | 1.92 |
  | 1 | 4.15 / 7.32 / 13.00 | 3.68 / 6.77 / 11.70 | 1.94 |

  No difference: the same setting varied more between rounds (6 threads: typical worst 8.25, 7.16, 7.80) than the settings did from each other. One tag is one decode task, so extra threads have nothing to share. The default stays 6; re-test with several tags per camera (the field case), where the decode tasks can spread out.
- **Camera stuck after rapid restarts.** After 4 PhotonVision restarts in 3 minutes, TopRight sent only corrupt frames: cscore logged "invalid JPEG image received from camera" 120 times a second, and nothing reached the pipeline. One more restart fixed it. `health-check.sh` now warns about this, and when fewer detectors report than cameras are plugged in.
- **Exposure 50 (5 ms), decision margin 15** (team-tuned), now the new-camera defaults (`photonvision-10`). `tests/flicker-check`: 0.6% average and 1% maximum frame-to-frame brightness change under the shop LEDs, so no flicker.

### Dashboard stream only when watched (`photonvision-15`, 2026-09-24)

PhotonVision's stream thread shrinks, colour-converts and draws on every frame for the dashboard, even when no browser is watching, and it used to get every frame (122 fps). Now `VisionModule` hands it a frame only while a stream has a viewer (cscore enables a source only while an MjpegServer client streams from it) or a snapshot is pending, and at most `SPECTRUM_STREAM_FPS` a second (default 30; 0 = every frame). Snapshots are never delayed.

| 2 cameras, hardware decode on | PhotonVision CPU | Stream |
|---|---|---|
| Before, no viewer | 0.49 cores | |
| After, no viewer | **0.43 cores** | |
| Before, one viewer | 0.52 cores | 121 fps |
| After, one viewer | **0.47 cores** | 30 fps |

Tested: snapshots with no viewer (the websocket `saveInputSnapshot`/`saveOutputSnapshot` commands the UI sends) still saved both images. They're 213x133, because snapshots were always taken from the shrunken stream image.

### Hardware JPEG decode: NVJPG (2026-09-24)

The camera JPEGs can be decoded on the Orin Nano's two NVJPG engines instead of the CPU, both gray (AprilTags) and colour (game pieces, driver mode, calibration). It's switched on with `08-select-detector.sh bos --mwbd 20 --jpeg nvjpg`, with libjpeg-turbo as the fallback. The research and the pitfalls are in [VISION-RESEARCH.md](VISION-RESEARCH.md).

- **How it works (gray).** It's the same Java call, `decodeMjpegGray`, so no jar change was needed for AprilTag cameras. The detector library hands each JPEG to `libspectrumnvjpg.so` (`detector/NvJpgDecoder.cc`):
  - libnvjpeg decodes into its own buffer (`IsVendorbuf`), and CUDA copies the Y plane into PhotonVision's Mat (0.24 ms).
  - libnvjpeg cycles through 4 buffers behind one fd number. Each gets its own CUDA registration, keyed by its dmabuf inode.
  - It's a separate library because libnvjpeg exports libjpeg-turbo's function names. `lib971apriltag.so` loads it with `dlopen(RTLD_DEEPBIND)`.
  - There's one decoder per camera thread.
  - **MJPEG mode (`cinfo.mjpeg_decode = TRUE`) is required.** Without it, libnvjpeg leaked ~250 KB every frame: PhotonVision grew to 5.6 GB and was OOM-killed twice (2026-09-24). NVIDIA's own NvJPEGDecoder class sets it. With it, each decoder takes ~180 MB once and then stays flat: 8 minutes in PhotonVision with 2 cameras, ~116,000 frames, RSS 1.078 → 1.092 GB.
- **How it works (colour, `photonvision-18`).** When the hardware decoder is on, `USBFrameProvider` takes the camera's raw JPEG for colour frames too, and calls `decodeMjpegBgr`. The hardware decodes to Y/Cb/Cr planes, then a CUDA kernel (`detector/nvjpg_bgr.cu`) converts them to BGR with libjpeg's own arithmetic ("fancy" chroma upsampling and the jdcolor.c tables). So the pixels are identical to cscore's decode, and the safety-net check can be exact. Only 4:2:2 JPEGs, which UVC cameras send, use the hardware; others go to libjpeg-turbo. With the decoder off (`hardwareJpegDecode()` false), colour frames stay on cscore's own path.
- **Measured in PhotonVision** (2 cameras at 122 fps, bench scene, no tags):

  | Decoder | PhotonVision CPU | Decode per frame |
  |---|---|---|
  | libjpeg-turbo | 0.84–0.86 cores | 2.0 ms |
  | NVJPG | **0.52 cores** | 2.6 ms |

  The engine takes ~2.3 ms whatever the scene; it already runs at its 499.2 MHz maximum. libjpeg-turbo's time grows with detail: 2.9 ms on our recorded frames, 2.0 ms on this plain bench scene. Detect times and fps didn't change.

  **Colour** (TopRight in driver mode at 120 fps, TopLeft on AprilTags): PhotonVision used **0.59 cores** with the hardware colour decode (3.1 ms a frame), against **1.12 cores** with cscore's own decode. That's about half a core saved per colour camera at 120 fps.
- **Memory cost:** ~180 MB per camera (libnvjpeg's decoder). With 4 cameras, PhotonVision should sit near 1.5 GB instead of ~0.7 GB.
- **Safety net.**
  - A frame the hardware can't decode goes to libjpeg-turbo. After a decode error the decoder is re-created.
  - Every 240 hardware frames per camera (~2 s), a low-priority thread decodes the same JPEG with libjpeg-turbo and compares every pixel. Any difference turns the hardware decoder off until PhotonVision restarts (`971 jpeg: HARDWARE DECODE DIFFERS`). Frames libjpeg-turbo warns about (corrupt ones) aren't compared.
  - A CUDA error in the decoder also turns it off. The detector's watchdog handles a broken context as before.
  - `health-check.sh` shows the decoder in use, its checks, fallbacks and any difference. It also warns when PhotonVision uses over 2.5 GB, or was OOM-killed since boot.
- **Logs.** A `971 jpeg` line every 10 s: frames/s and ms for each decoder, fallbacks, and checks since start, plus a `colour:` clause when colour frames were decoded.
- **Tested.**
  - `tests/jpeg-hw/run.sh`: every frame of the Rewind recordings identical to libjpeg-turbo, in gray and in BGR; bad input (truncated, corrupted, garbage, not a JPEG, wrong size, empty), each followed by a good frame that decodes on the hardware; format changes; 4 decoders at once; and memory growth (fails over 64 MB). **Run it after every L4T update.**
  - Colour content: our cameras are mono, so `tests/jpeg-hw/make-colour-recordings.py` pans across real colour photos already on the Jetson (Ubuntu wallpapers, OpenCV samples) and writes 4:2:2 recordings at quality 50/80/95 plus 4:2:0 and 4:4:4 ones. All 2,038 colour 4:2:2 frames came out identical to libjpeg-turbo; 4:2:0 and 4:4:4 were refused, as they should be. **Re-run it with a real colour camera's Rewind recording when one arrives.**
  - Safety net: `touch /tmp/spectrum-jpeg-fault` changes one pixel of every hardware frame. The next check caught it, the hardware turned off, and both cameras stayed at 122 fps.
  - Sticky CUDA fault (`echo sticky > /tmp/spectrum-971-fault-every`): the decoder fell back to libjpeg-turbo, the watchdog restarted PhotonVision after 1 s, and it came back on the hardware.
- **Switches.**
  - `SPECTRUM_JPEG_DECODER=nvjpg`, set by `08-select-detector.sh --jpeg nvjpg`. Without it, libjpeg-turbo.
  - A/B without a restart: `echo turbo > /tmp/spectrum-jpeg-decoder` (or `nvjpg`). It's read every 2 s; `rm` it to go back to the setting.
- **Rollback.** `08-select-detector.sh bos --mwbd 20` without `--jpeg` goes back to libjpeg-turbo, and colour frames go back to cscore. The libraries from before are `/usr/lib/lib971apriltag.so.pre-nvjpg` (before any hardware decode) and `*.pre-colour` (before the colour path and the leak fix).
- **Measured and dropped: the detector reading the decoder's buffer directly.** bos's `Detect(host, device)` takes a GPU pointer, and detections were identical on 2,856 frames. Against this path it skips the copy into PhotonVision's Mat (0.27 ms) and the detector's upload (0.16 ms). But the detector's first kernel reads the decoder's buffer 3x slower (0.21 against 0.07 ms), and its CPU step reads that uncached buffer too (+0.13 ms). Net: ~0.15 ms and ~0.1 ms of CPU a frame, plus ~0.15 ms more from PhotonVision skipping its full-size copies. Not worth a PhotonVision patch. (End-to-end totals in the harness were noisy: our test decodes queued behind PhotonVision's live decodes on the same engines. The per-step times above are consistent between runs.)
- **Not done yet:** 4 real cameras, and a real colour camera.

### CUDA error handling (bos build)

- `patches/bos-01-nonfatal-cuda.patch`: `CHECK_CUDA` throws instead of `LOG(FATAL)`.
  The JNI skips the frame and rebuilds the detector on the next one.
- If frames fail continuously for 1 s, the JNI calls `_exit(1)` and systemd restarts
  PhotonVision. It uses `_exit`, not `abort()`: SIGABRT went through the JVM crash
  handler and Apport, which took 28 s and wrote a 156 MB `/var/crash` report.
- The service runs Java with `-XX:-CreateCoredumpOnCrash` (06-install-fork-jar.sh),
  so real native crashes also restart quickly.
- Measured with fault injection (`echo N > /tmp/spectrum-971-fault-every`):
  - **1 error per 100 frames:** no restart, 99% of frames still detected, ~59 fps.
  - **Every frame failing:** exits after 1.7 s, detecting again 6.2 s later. That is
    about 8 s total, vs about 62 s before the fixes.

Still open: `use_neon` (a CPU NEON threshold absl flag) is untested and off.

### Jetson telemetry and camera mount estimate (2026-09-24)

`photonvision-16` and `photonvision-17`, plus `nativeJpegStatus` in `detector/GpuDetectorJNI.cc`.
Robot-side use is in [issue #10](https://github.com/Spectrum3847/2026-FM-SystemCore/issues/10).

**Jetson, `/photonvision/jetson/`, every 1 s** (`JetsonTelemetry`):

| Topic | Type | Source |
|---|---|---|
| `gpuLoadPct` | double | `/sys/devices/platform/bus@0/17000000.gpu/load` (per mille) |
| `cpuTempC`, `gpuTempC`, `tjTempC`, `socTempC` | double | thermal zones `cpu-`, `gpu-`, `tj-thermal`; hottest of `soc0..2-thermal` |
| `fanRpm` | double | hwmon `pwm_tach` `rpm` |
| `powerW`, `cpuGpuPowerW`, `socPowerW` | double | INA3221 rails `VDD_IN` (board input), `VDD_CPU_GPU_CV`, `VDD_SOC` (mV x mA) |
| `jpegDecoder` | string | `nvjpg` or `libjpeg-turbo` (the decoder in use) |
| `jpegHardwareOff` | boolean | the hardware decoder was switched off (a check differed, or CUDA failed) |
| `jpegChecksOk`, `jpegChecksDiffer` | integer | hardware-vs-CPU frame checks since start |
| `throttle` | string | why the Jetson is slowing itself down: `None`, `OVER-CURRENT`, `HIGH TEMP (cpu, ...)`, `CPU CLOCK CAPPED`, `GPU CLOCK CAPPED`, or `Prev. over-current (N)` (`photonvision-20`, below) |
| `overCurrentEvents` | integer | soctherm over-current throttle events since boot |
| `heartbeat` | integer | +1 per publish; a stalled value means the telemetry (or PhotonVision) stopped |

- Topics the hardware doesn't have aren't published.
- The JPEG topics need the detector library with `nativeJpegStatus` (rebuild with 07, then
  `08-select-detector.sh bos --mwbd 20`).
- The hardware decoder (`--jpeg nvjpg`) leaked memory inside PhotonVision until it was switched to
  libnvjpeg's MJPEG mode (2026-09-24, see Hardware JPEG decode). While it's off, `jpegDecoder` reads
  `libjpeg-turbo`.
- PhotonVision's own metrics (`/photonvision//metrics/<host>`: CPU temperature and use, RAM,
  disk, uptime) are unchanged.

**Per camera, `/photonvision/<camera>/health/`, every 1 s** (`CameraHealthPublisher`):
- `fps`: pipeline results per second.
- `pipelineMs`, `pipelineMsMax`: average and worst over the last second.
- `latencyMs`: average, capture to result.
- `frames`: total since start.
- `decodeFailures`: total frames our decoder rejected, counted in `USBFrameProvider`. A rising
  count means corrupt JPEGs are getting through.
- The stuck-camera case (see "Decode speedup") shows as `fps` near 0 instead: cscore drops the
  corrupt frames itself, before PhotonVision sees them.

**Per camera, `/photonvision/<camera>/mount/`, every 0.5 s over the last 2 s of multi-tag frames**
(`MountEstimatePublisher`):

| Topics | Meaning |
|---|---|
| `heightM`, `pitchDeg`, `rollDeg` | Means. The camera's pose on the field; with the robot level on the floor, the same as its mount on the robot |
| `heightStdM`, `pitchStdDeg`, `rollStdDeg` | Spread over the window |
| `fieldXM`, `fieldYM`, `fieldYawDeg` | Camera position and heading on the field (yaw is a circular mean), for robot code to combine with its own pose |
| `reprojErrorPx` | Mean multi-tag reprojection error |
| `samples` | Multi-tag frames in the window. 0 means nothing else is updated |

- Angles follow WPILib's `Rotation3d`, like `robotToCamera`: positive pitch points the camera down,
  so a camera tilted up has negative pitch.
- The Targets tab shows the same height, pitch and roll from the UI's 100-sample buffer.
- The robot's origin must be on the floor (WPILib's convention) for the height to match
  `robotToCamera`'s z.

**Settings page** (`photonvision-19`):
- A **GPU Usage** chart under CPU Usage, from the same GPU load file as `gpuLoadPct`.
- It's shown only where the GPU reports its load.
- It's added to the metrics record the UI gets (`gpuUtil`), but not to PhotonVision's
  NetworkTables protobuf. Robot code reads `/photonvision/jetson/gpuLoadPct` instead.
- Checked on the websocket: 10–13% with 2 cameras.

**Throttle reason** (`photonvision-20`, `JetsonThrottle`): the Jetson's version of the Raspberry Pi's
under-voltage and high-temperature flags. It reads only files that don't need root:

| Reason | Source |
|---|---|
| `OVER-CURRENT` | soctherm's over-current event counters (hwmon `soctherm_oc`, `oc1..3_event_cnt`) went up in the last 10 s. The chip throttles when its supply current spikes, e.g. when the robot's battery sags. |
| `Prev. over-current (N)` | N such events since boot, none recently |
| `HIGH TEMP (cpu, gpu, ...)` | a thermal throttle alert is active (`*-throttle-alert` and `hot-surface-alert` cooling devices) |
| `CPU CLOCK CAPPED`, `GPU CLOCK CAPPED` | the thermal framework is holding the clock below its maximum (`cpufreq-cpu*`, `devfreq-17000000.gpu` cooling devices) |

- It fills the Settings page's **CPU Throttling** row and `cpu_thr` in PhotonVision's own
  NetworkTables metrics, via `SystemMonitorJetson`.
- It's also published as `/photonvision/jetson/throttle`.
- Found on the bench: 3 over-current counters, 9 alerts, 2 CPU and 1 GPU clock caps. Reads `None`
  with the fan at full speed; checked on the websocket.

**Bench check:** `tests/jetson-telemetry/run.sh` runs a NetworkTables server on the Jetson and prints
every topic above. Not run yet: the NT topics are published by the running build but haven't been
read back. Set PhotonVision's NT server address to 127.0.0.1 first, and set it back to 8515
afterwards.

### Bad tags, calibration, settings snapshots, copy settings, line-fit knob (2026-09-24)

**`photonvision-22`: tags left out of multi-tag** (`ExcludedTags`). One list for every camera, the
union of two sources:
- the Settings page's AprilTag Field Layout card, saved in
  `photonvision_config/spectrum/excluded-tags.txt` (included in settings exports);
- robot code: `/photonvision/excludedTags` (integer array).

How it behaves:
- Both AprilTag pipelines filter the targets they pass to `MultiTargetPNPPipe`. Excluded tags are
  still reported, with single-tag poses.
- The combined list is published as `/photonvision/excludedTagsActive`.
- REST: `GET/POST /api/excludedTags`, `{"saved": [7, 12]}`.
- **Checked:** save, persist, log, clear, and a 400 for bad input.
- **Not yet checked:** the multi-tag result with tags in view.

**`photonvision-23`: calibration.**
- **Upstream #2437:** 100 snapshots minimum.
- **Auto Snapshots:** a separate toggle, one snapshot request a second; the backend keeps a frame
  only when it finds the board. **Take Snapshot** still works on its own. The idea is from upstream
  #2149, which starts calibrating and loops snapshots behind one button.
- **Board sizes in mm** (upstream #2479; the backend still gets inches).
- **Our board is the default:** ChArUco `Dict_5X5_1000`, 30/22 mm, width 12, height 9.

**`photonvision-24`: settings snapshots for the robot log** (`CameraSettingsPublisher`,
`JetsonSettingsPublisher`). Rebuilt every 5 s and published with `keepDuplicates(false)`, so a value
only goes out when it changes.
- **`/photonvision/<camera>/settingsJson`:**
  - camera, pipeline index, enabled, FPS limit, video mode, quirks
  - the calibration in use: resolution, fx, fy, cx, cy, distortion coefficients, snapshot count,
    lens model
  - `controls`, the raw UVC values: exposure, brightness, contrast, gamma, sharpness, gain, white
    balance, backlight, power-line frequency, autofocus
  - `pipeline`, the full pipeline settings as PhotonVision saves them
- **`/photonvision/jetson/settingsJson`:**
  - PhotonVision version and build date, hostname
  - every `SPECTRUM_*` environment variable, plus the `/tmp` runtime overrides for the JPEG
    decoder and CUDA wait
  - the excluded tags
  - the field layout: tag count, size, and a SHA-256 fingerprint of its tags
  - the uvcvideo `payload_cap`
- **Rewind:** `session.json` gets `settings` from the start of the recording, and `settingsAtEnd`
  if they changed.
- **Checked** in a bench recording's `session.json`:
  - TopRight's calibration: fx 737.0, 41 snapshots, 8 coefficients
  - its controls: contrast 32, gamma 150, sharpness 5, autofocus 0
  - its pipeline: exposure 50

**`photonvision-25`: copy settings** (`VisionModule.copySettingsFrom`,
`POST /api/settings/copySettings`).
- **Groups:**
  - `camera`: exposure, auto exposure, brightness, gain, white balance, stream divisor,
    `blockForFrames`
  - `resolution`: only when both cameras list the same video modes
  - `apriltag`: tag family, decimate, blur, threads, refine edges, iterations, hamming, decision
    margin
  - `output`: 3D, multi-tag, single-tag fallback, drawing, max targets
  - `objectDetection`: confidence, NMS, model
- **How:** fields are copied by reflection, and fields the other pipeline type lacks are skipped.
  If the target pipeline is running, it's re-applied with `setPipeline`, then everything is saved
  and broadcast.
- **UI:** the pipeline menu's **Copy settings from…**, optionally into the same pipeline number on
  every camera.
- **Checked:** TopLeft → TopRight (identical settings) copied 22 fields, and the object-detection
  fields were skipped. Copying a pipeline onto itself, or an unknown group, returns 400.

**Detector `max_line_fit_mse`** (`SPECTRUM_971_MAX_LINE_FIT_MSE`, `08-select-detector.sh --mse N`).
- **Default 10** (AprilTag's), shown in the "971 library loaded" line.
- **What it does:** the GPU line-fit filter rejects a quad if any side's fit error is above it (bos
  `apriltag.cc` → `line_fit_filter.cc`).
- **Upstream #2138** lowers PhotonVision's CPU detector to 2.5, so tags cut off at the image edge
  aren't detected, with little range loss in their tests.
- **Not changed yet:** test it with tags in view first.
- **Deployed build passes** `tests/jpeg-hw/run.sh`.

**`photonvision-26`: tuning guide.**
- **Where:** a collapsible "Tuning guide: what to set, in order" at the top of the Input tab (`TuningGuide.vue`).
- **AprilTag pipelines, in order:**
  1. resolution
  2. auto exposure off
  3. exposure, as short as tags still decode (5 ms), with blur numbers and the 120 Hz flicker rule
  4. brightness 100
  5. decision margin 15
  6. leave the rest
  7. 3D and multi-tag
  8. check on the field, and copy to the other cameras
- **Object Detection pipelines:** a shorter version.
- **Tooltips:** auto exposure, exposure, brightness, gain, low latency, resolution, stream resolution and both decision-margin sliders now lead with the recommended value.
- **Calibration card:** says its long exposure is only for a still board.
- **Blur numbers:** from f ≈ 737 px: 3 rad/s × 5 ms = 0.015 rad ≈ 11 px; 20 ms ≈ 44 px.

### A camera stuck at the wrong resolution (2026-09-24, `photonvision-27`)

**What happened** after a PhotonVision restart at 14:19:
- TopRight streamed 320x240 MJPEG (`v4l2-ctl --get-fmt-video`) while PhotonVision and cscore both
  believed 1280x800.
- At every start, PhotonVision's `setVideoMode` races cscore's connect-time "restoring video mode"
  (logged as "Failed to set video mode!"). Usually the result is still 1280x800; this time it wasn't.
  About 1 in 20 restarts today.
- The direct decode correctly returned "size mismatch" (-3). After 30 in a row it permanently fell
  back to cscore's conversion, which *upscaled* 320x240 to 1280x800.
- Detection kept running at 122 fps, with less range and 1.7 cores instead of 0.5. Found by the
  JPEG-decode session.

**Fix:** `DirectDecode`, one per path, for gray and BGR.
- **A size mismatch never falls back.** The frame is dropped. Once the mismatch lasts 1 s,
  `USBFrameProvider.reconnectForVideoMode` sets the camera's connection strategy to `kForceClose`,
  waits for it to close (up to 1 s), and returns it to `kAutoManage` (PhotonVision's default).
- **Why reconnect:** on reconnect, cscore pushes its `m_mode` to the device ("restoring video
  mode"). Setting the same mode again is a no-op: `UsbCameraImpl::DeviceCmdSetMode` returns when the
  mode is unchanged.
- **Throttled** to every 3 s, and logged as a warning.
- **Real decode failures** (30 in a row) still fall back, but the direct path is retried every 10 s,
  and one failed retry switches straight back.
- **A detector library without the decoder** is never retried.
- **Unit tests:** `DirectDecodeTest` passes 4/4.
- **After deploy:** both cameras at 1280x800, nvjpg 242 frames/s, PhotonVision 0.44 cores.
- **Not yet seen in action:** the race is rare, so watch the log for "reconnecting it so the mode is
  applied again".
- **Health check:** the JPEG session's check now fails any camera whose V4L2 format differs from
  what PhotonVision set.

### Hidden camera controls and stuck-camera recovery (2026-09-24, `photonvision-28`, `-29`)

**`photonvision-28`: contrast, gamma, sharpness, backlight compensation.**
- **Settings:** new `CVPipelineSettings` fields `cameraContrast`, `cameraGamma`, `cameraSharpness`,
  `cameraBacklightCompensation`. -1 means the camera's default.
- **Applying them:** `setPipeline` applies them on every switch; -1 restores the default, so
  pipelines stay independent. They're set in the camera's own units (cscore's `raw_*` properties,
  clamped to the camera's range).
- **UI:** `UICameraConfiguration.extraControls` lists the controls the camera has (key, label,
  min, max, step, default, value). The Input tab shows a slider for each, or a switch for on/off
  ones. They're in the copy-settings Camera group.
- **Thriftiest Cam ranges** (from the camera):
  - contrast 0–95 (default 32)
  - gamma 100–300 (default 150)
  - sharpness 1–10 (default 5)
  - backlight compensation 0–1 (default 1)
- **Checked:** setting `cameraContrast` 40 over the websocket gave `contrast: 40` in `v4l2-ctl`;
  -1 restored 32.

**`photonvision-29`: stuck-camera recovery** (`StuckCameraWatchdog`, in `USBFrameProvider`).
- **Usable frames:** every `getInputMat` result counts, meaning a non-empty image with a capture
  time, from any decode path.
- **Recovery:**
  1. No usable frame for 3 s while the camera is connected: reconnect it (`kForceClose` →
     `kAutoManage`).
  2. Still none 5 s after that: a USB-level reset. The USB device's sysfs `authorized` is set to 0,
     then after 1 s to 1, via `/sys/class/video4linux/videoN/device/..`. It re-enumerates like a
     replug.
  3. After that, a USB reset every 30 s while it stays stuck.
- **Shared throttle:** the patch-27 mode-fix reconnects use the same throttle, so the two never
  reconnect at once.
- **Health:** `/photonvision/<camera>/health/recoveries` counts them.
- **Test hook:** `/tmp/spectrum-camera-stuck-test`, listing camera names, drops those cameras'
  frames. Delete it to end the test.
- **Bench test, TopRight:**

  | Time | What happened |
  |---|---|
  | 14:45:19 | Test started: TopRight's frames dropped |
  | +3.0 s | Reconnect |
  | +8.0 s | USB reset of `1-2.3` |
  | +9.1 s | Kernel re-enumerated it: "Found UVC 1.00 device Thrifty", the payload cap re-applied, "authorized to connect". cscore reconnected |
  | end of test | "delivering frames again, after a USB reset" |

  Afterwards the health check passed: both cameras streaming 1280x800 MJPG as set, NVJPG 242
  frames/s with checks ok, 122 fps each.
- **Unit tests:** `StuckCameraWatchdogTest` 5/5 and `DirectDecodeTest` 4/4.

### Field calibration page (2026-09-24, `photonvision-30`)

A **Field Calibration** page in the web UI (`/#/fieldcal`) for the whole field calibration, done
on the Jetson. The user guide is in the README; how the solver works is in
[tools/fieldcal](../tools/fieldcal/README.md).

- **Backend:** `org.photonvision.fieldcal.FieldCalibration`, a singleton.
  - **Recording** is a Rewind bench recording labelled `fieldcal`. `RewindManager.setManual` now
    takes a label.
  - **Live guidance:** a result consumer on every `VisionModule` (a no-op unless recording).
    - Samples each camera's tag corners every 100 ms.
    - More than 1.5 px of motion, or a completely different set of tags, counts as moving.
    - A spot counts after 1.5 s with no motion from any camera.
    - Per spot, it records which camera saw which tags, and each camera's field position: from
      multi-tag, else the least ambiguous single tag (ambiguity < 0.15).
  - **Solve:** `nice -n 10 python3 -m fieldcal solve` from `/opt/spectrum/fieldcal`, with the
    current PhotonVision layout, the 971 replay (`FIELDCAL_971_DETECT`), and robot code's mounts
    as `--cad`.
    - It inherits PhotonVision's `SPECTRUM_971_*` settings.
    - Output goes to `/opt/photonvision/fieldcal/runs/<recording>/`.
    - "Stop and solve" waits 1.5 s so the recorders finish their files.
  - **Mounts:**
    - read: `/photonvision/<camera>/robotToCamera` (`Transform3d` struct, from robot code);
    - published after a solve: `/photonvision/<camera>/fieldcal/robotToCamera`.
  - **Apply:** saves the layout in use to `/opt/photonvision/fieldcal/layout-backups/`, then loads
    the corrected layout the way an uploaded layout is loaded, and restarts.
  - **Undo:** loads the newest backup and deletes it.
- **Settings tuner:** `CameraSettingsTuner`, run with the robot still, on one camera or on all.
  - **Per camera:** each camera's last results are kept until it's tuned again, and **apply**
    works per camera, so the robot can be turned so each camera faces tags in turn.
  - **Locks:** locked controls are skipped and left as they are. They're stored on the Jetson in
    `/opt/photonvision/fieldcal/locks.json` (`setLock`), so every browser shows the same locks
    and the tuner uses them. A `"locked"` map in the tune request overrides them for that run.
  - **By hand:** `setControl` writes one control into the pipeline in use, applies it and saves,
    as the Input tab does. -1 restores the camera's default for contrast, gamma, sharpness and
    backlight. It's refused while that camera is being tuned.
  - **Preview:** while the page is being polled (within 5 s), every camera's result consumer
    records the tags in view and their decision margins, 4 times a second, for the per-camera
    cards. The page also shows each camera's processed stream.
  - **Checked on the bench:** a manual contrast of 40 reached the camera (`v4l2-ctl`: 40), and -1
    put back the default (32) and the saved pipeline's -1; a locked single-camera tune ran on
    TopLeft alone.
  - **Baseline:** the tags each camera finds in at least 60% of frames over 1.5 s.
  - **Each setting:** 0.4 s to settle, then 0.9 s of frames. It records, for the baseline tags,
    how often each is found, the mean decision margin (`TrackedTarget.getDecisionMargin()`, new)
    and the corner jitter, plus the image's mean brightness.
  - **Order:**
    - exposure: 8 values, 0.18–2x the current; keep the shortest that finds the baseline tags as
      well as the best (within 3%) with at least 85% of the best margin;
    - then gain (if the camera has it), brightness, contrast, gamma, sharpness and backlight
      compensation, each kept only for a 5% better margin;
    - a control with no effect on brightness or margin is reported as such.
  - **End:** the camera goes back to its pipeline's settings. **Apply** writes the
    recommendations into the pipeline in use and saves.
  - **Moving robot:** a tag's centre moving more than 3 px from the baseline stops the sweep.
- **API:**
  - `GET /api/fieldcal`: state, live guidance, tuning, solve log, mounts, recordings, runs.
  - `POST /api/fieldcal {"action": ...}`: start, stop, solve, cancel, tune, cancelTune,
    applyTuning, apply, undo.
  - `GET /api/fieldcal/file?session=&name=`: report.md, results.json, corrected-layout.json,
    mounts.json.
- **3D view** (`FieldCal3D.vue`, three.js already in the UI):
  - **Field:** *FIRST*'s official field CAD by default, converted by `tools/fieldmodel` into
    `assets/field-models/2026-rebuilt.glb` (2.1 MB, meshopt). It's loaded with `GLTFLoader` +
    `MeshoptDecoder` from `fieldmodels/`, which the jar build copies in.
    - `lib/FieldModel.ts` moves each element node (`kind@tags#...`) to its tags in the layout in
      use, and the perimeter's four sides (`@x0`/`@xL`/`@y0`/`@yW`) to the layout's field size.
    - Placed with WPILib's welded layout, every element lands where the build put it (0.001 mm).
    - With AndyMark's layout, the elements move 1.6–3.6 cm and the far walls 2.3 and 2.6 cm.
  - **Fallback:** if the model doesn't load, simplified elements from `lib/FieldElements.ts`,
    sized from the game manual. Each is placed by the tags mounted on it, so the AndyMark and
    welded layouts both work, and after a calibration each element sits where its tags really are:
    - hubs: tags 18–21, 24–27 and 2–5, 8–11;
    - trenches: tags 17/28, 22/23, 1/12 and 6/7, against the guardrail;
    - bumps: between each hub and its trenches;
    - towers: tags 31/32 and 15/16;
    - outposts: tags 29/30 and 13/14.
  - **Mouse:** Onshape's defaults (right-drag turns, middle or Ctrl + right-drag pans, the wheel
    zooms toward the cursor); left-drag also turns. The view can't tilt below the carpet.
- **Install:** `scripts/jetson/13-build-fieldcal-detect.sh --install`.
- **Checked on the bench (no tags in view):**
  - start, live status, stop-and-solve: the replay ran at 560+ fps and reported "nothing to solve";
  - tuning reported "no tags in view" and read the Thriftiest's control ranges;
  - a synthetic 4-camera recording solved through the API;
  - apply then undo: the original layout came back exactly (32 tags, zero difference).
- **Not yet checked:** the page in a browser, the tuner with real tags, and live guidance with
  real tags.

### USB bandwidth: one shared budget, trouble detection, the Camera Matching card (2026-09-24, `photonvision-31`, `-32`)

**The budget.** Measured by setting cameras' alternate settings directly over usbfs, with PhotonVision
stopped (`USBDEVFS_SETINTERFACE`; the host controller answers ENOSPC when a reservation doesn't fit).
The Jetson has one xHCI controller (`3610000.usb`): one USB 2.0 bus, one USB 3 bus.
- **Shared:** its USB 2.0 root ports are USB-C (1-1), the USB-A hub (1-2) and M.2 Key E
  (1-3, the Bluetooth). They share one budget.
- **The numbers:** 3072 on USB-C plus 3060 on USB-A fit, but then a second 3060 on USB-A was
  refused, although USB-A alone takes two.
  - Fit: 6120, 6132, 6240, 6720.
  - Refused: 7400, 7520.
  - So the budget is about 6700–7400 bytes per microframe, above USB 2.0's nominal 80% (6000).
- **Consequences:** moving a camera to USB-C doesn't help.
  - **A second USB 2.0 bus:** only a PCIe USB controller in the empty M.2 Key M 2230 (x2) slot. The
    kernel has `xhci-pci` as a module.
  - **Or avoid USB 2.0:** USB 3 cameras (CSI isn't planned).

**The driver** (`kernel/uvcvideo-payload-cap.patch`, rebuilt by `11-uvcvideo-payload-cap.sh`):
- **Writable cap:** `payload_cap` is now `module_param_string`, 0644, and parsed under
  `kernel_param_lock`.
- **Per-port entries:** entries can be `port:bytes` (e.g. `1-2.4:944`), matched against
  `dev_name(udev)`. A port entry wins over `vid:pid:bytes`; 0 means uncapped.
- **Byte counters:** the debugfs `stats` file gains `bytes:` (USB payload, headers included) and
  `frame bytes: max N, recent max N` (the largest in this and the last 256-frame window). These are
  counted in `uvc_video_stats_decode` / `_update`, which run for every packet and frame anyway.
- **Applying a change:** the cap applies at the next probe.
  - A cscore reconnect (`kForceClose`, then `kAutoManage`) did **not** apply it: the camera was still
    at 1600 bytes 4 s after a change to 256.
  - A USB reset (sysfs `authorized` 0/1) does, in 1.5–2 s.
- **Reinstalling:** `11-uvcvideo-payload-cap.sh` keeps port entries from
  `/etc/modprobe.d/90-spectrum-uvcvideo.conf`.

**`photonvision-31` (trouble):**
- **Detection:** `UsbTrouble` reads every kernel line (via `KernelLogLogger`) for "Not enough
  bandwidth" and enumeration failures, by port.
- **Recovery:** a camera with no frames and a bandwidth failure on its port in the last 90 s
  reconnects at most every 60 s, with no USB reset (a reset can't add bandwidth).
- **Reporting:** the reason is published as `/photonvision/<cam>/health/problem` (a 1 s timer, so
  cameras without frames report too).
- **Not yet checked live:** the `problem` topic, and that no resets happen during a real bandwidth
  failure (all 5 cameras have fit since).

**`photonvision-32` (the Camera Matching card):**
- **Backend:** `UsbBandwidth` (photon-core `common/hardware`) serves
  `GET /api/usb/bandwidth`. PhotonVision runs as root, so it can read debugfs and write the
  parameter.
  - **Survey:** every high-speed USB video device's alternate settings (parsed from sysfs
    `descriptors`), its current reservation (`bAlternateSetting`), and its cap.
  - **Usage:** fps and bytes/s from the driver's counters, as deltas between polls at least 0.5 s
    apart.
- **Changing an allocation:** `POST {port, bytes}` checks the new total against the budget, then
  writes the parameter and the modprobe conf. It then calls `USBFrameProvider.applyUsbBandwidth`,
  which does a USB reset and confirms the new reservation within 6 s.
- **UI:** `components/app/usb-bandwidth-card.vue`, fed by `lib/UsbBandwidth.ts`, a shared 1.5 s
  poller. It shows the budget bar, a table per camera, and a select per camera (settings over the
  budget are disabled), plus a "USB bandwidth" row on each camera card.
- **Checked:**
  - all 5 cameras shown;
  - allocation changes from the page (Allen's: the colour camera to 256, both Global Shutters to
    512) survived a PhotonVision restart;
  - 256 → 800 → 256 applied in 2.2 s and 1.5 s;
  - `tests/jpeg-hw/run.sh` passed afterwards.
- **Starved cameras** (README table): the Thriftiest (1bcf:28c5) at 640 bytes and the colour camera
  (32e4:62f0) at 128 bytes both kept their frame rate. They compressed harder instead: frames 14%
  smaller, 96–97% of microframes busy, no errors.
- **Measured before the card**, over 5 s with the kernel's `v4l2_dqbuf` trace event:
  - TopLeft: 121 fps, 58.6 KB frames, 7.1 MB/s of 10.2;
  - TopRight: 47.5 KB, 5.8 MB/s;
  - the Global Shutters: 60 fps, 39–57 KB, 2.4–3.4 MB/s;
  - the colour camera: 30 fps, 36.6 KB, 1.1 MB/s of 12.8.

### USB hub reset, stuck Thriftiest Cams, a hung reboot, and the log flood (2026-09-25)

**The test** (`tests/usb-hub-reset/run.sh`): set the USB-A hub's sysfs `authorized` (1-2) to 0 for 2 s,
then back to 1, with 4 cameras streaming.
- **Global Shutter cameras (32e4:0144)** re-enumerate and stream again by themselves.
- **Thriftiest Cams (1bcf:28c5)** never answer again.
  - The kernel logs "device descriptor read/64, error -110", then after the retries "unable to
    enumerate USB device".
  - The USB link comes up (high speed), but the camera doesn't answer control transfers.
  - A deauthorized hub stops sending SOFs, so its devices see an idle bus (suspend). The
    Thriftiest's firmware apparently doesn't recover from that without a power cut.
  - A device-level reset (the camera's own `authorized`, as `photonvision-29` and `-32` do) is
    fine: TopRight went through it twice.
- **What didn't revive them:**
  - the kernel's own port power cycle;
  - `ClearPortFeature(PORT_POWER)` from usbfs for 3 s and 10 s, on one port and on all four;
  - rebinding `tegra-xusb` (a full controller reset);
  - a warm reboot.
- **Why:** the hub descriptor claims per-port power switching (`wHubCharacteristic` 0x00a9), but
  no port loses its 5 V.
  - Every `usbN-*-vbus` supply in the device tree resolves to `regulator-fixed`,
    `regulator-always-on` supplies (`VDD_5V0_SYS`, `VDD_AV10_HUB`), with no GPIO.
  - The Global Shutters on switched-off ports got -71 protocol errors but never a disconnect.
- **What revives them:** a real power cut, by replugging or a cold power cycle.
- **Retries:** each stuck port holds up the hub's other ports.
  - With usbcore's default 5 s `initial_descriptor_timeout`, the Global Shutters were back after
    65 s and 86 s.
  - At 1 s (`09-robot-tuning.sh` step 10, a tmpfiles.d write to the module parameter), both were
    back after ~41 s, about 20 s of retries per stuck port.
  - `use_both_schemes=0` would roughly halve that again; not tried.

**The hung reboot.** `systemctl reboot` right after the test, with both Thriftiest Cams stuck, never
came back.
- **What we know:**
  - The fan fell from full speed, so the SoC reset. It never returned to full, so Linux didn't
    finish booting.
  - A cold power cycle fixed it.
  - Allen has seen it once before.
- **The log:** journald wrote nothing after 03:48:31, although the system ran until the reboot at
  ~03:50. `sudo` entries from that time are missing too. So there's no record of the shutdown.
- **Suspected, not confirmed:** UEFI's USB scan waiting on the stuck cameras.
- **Changed:** `RebootWatchdogSec=30s` (was 10 min), so a hung shutdown resets itself. A hang in the
  boot firmware isn't covered; to find it, repeat the test with a DisplayPort monitor plugged in.

**The log flood** (`photonvision-33`).
- **What happened:** with cameras gone, each frame provider logged "Error grabbing image: timed out
  getting frame" and `CpuImageProcessor` printed "Input was empty!", for every attempt.
  - That was ~20,000 lines a minute: 39 MB journal files every 3 minutes, with journald
    suppressing ~2,500 more every 30 s.
  - At the old 300 MB `SystemMaxUse`, that kept ~25 minutes of history.
- **Now:**
  - Both log the first failure, then at most one line per 5 s with a count ("and 248 more since
    the last message"). The hub test logged 2,065 lines in a minute.
  - The journal keeps up to 2 GB.
  - Still frequent: cscore's own "Attempting to connect" lines, about 300 a minute with cameras
    stuck. Acceptable.
- **The "not answering" problem:** a camera that isn't connected, while the kernel has logged a
  connect failure on its port in the last 90 s, now gets the health `problem` "not answering on USB
  port X: replug it, or power-cycle the robot", logged once.

**USB controller watchdog** (`14-usb-watchdog.sh`, `usb-watchdog.service`, `usb-watchdog.py`).
- **Trigger:** it follows `journalctl -k -f` for "xHCI host controller not responding", "assume
  dead", "HC died" or "Host halt failed".
- **Action:** it rebinds `tegra-xusb` (3610000.usb), at most once a minute, then reports which
  cameras came back within 60 s. It never reboots.
- **Tested:** with a faked line on `/dev/kmsg`, it reset the controller in ~2 s. The Global
  Shutters came back; the stuck Thriftiest Cams didn't, as expected.

**Focus score** (`photonvision-34`, `FocusMeter`).
- **What it measures:** the variance of the Laplacian of the input image (gray; colour is converted
  first) on a 3x3 grid, at most 5 times a second, only within 3 s of a request.
- **Cost to the vision thread:** one image copy. The rest runs on its own thread.
- **API and UI:** `GET /api/focus?camera=UNIQUE_NAME[&reset=1]` returns
  `{grid, centre, best, width, height, ageMs}`, and 204 before the first reading. The Camera page's
  Focus card shows each cell against its own best since Reset.
- **Checked:** TopLeft returned readings about 5 times a second at 1280x800.

### Four Thriftiest Cams: CUDA's lock and per-frame event timing (2026-09-29, `bos-03`)

First run with 4 Thriftiest Cams (TopLeft, TopRight, BottomLeft, BottomRight; all 1280x800 MJPEG at
122 fps, no tags in view, no dashboard streams, hardware JPEG decode on). PhotonVision used 2.2 cores
and the GPU sat at 40–45%, against 0.8 cores and 17% on 2026-09-26 (two cameras at 61 fps then).

- **Scaling, unplugging one camera at a time** (same boot, 10 s windows):

  | Cameras | Detect | PhotonVision CPU | Cross-core wakeups (IPI1) | GPU |
  |---|---|---|---|---|
  | 1 | 1.45 ms | 0.38 cores | 3,000/s | 7% |
  | 2 | 1.8 ms | 0.65 | 4,100/s | 13% |
  | 3 | 2.2 ms | 1.1 | 6,500/s | 22% |
  | 4 | 4.4 ms | 2.2 | 20,000/s | 40% |

- **Where the CPU went:** more than half was kernel time (1.15 of 2.0 cores), in the 4 camera
  threads. `ipi:ipi_raise` tracing showed ~40 cross-core wakeups per frame, 82% of them futex
  handoffs, 44% from priority-inheritance unlocks. The 4 camera threads handed a lock around in
  strict rotation every 15–20 µs. gdb stacks: one camera thread in `cudaEventSynchronize` →
  `ioctl` (libnvrm_gpu), the others in `futex_lock_pi` inside `cudaEventSynchronize` and
  `cudaLaunchKernel`. That is libcuda's context lock, which every camera shares (one CUDA context
  per process).
- **Ruled out:** dashboard streams (closed: same load), hardware JPEG decode (off: kernel time
  −0.2 cores, the wakeups −10%), detector threads (1/2/6: no change), CUDA wait mode (`spin`: no
  change), a reboot (no change).
- **The cause:** at the end of every frame, `GpuDetector::Detect()` walked 22 CUDA events, calling
  `cudaEventSynchronize` and `cudaEventElapsedTime` on each, for a `VLOG(1)` timing report that is
  never printed. 16 of those events are recorded only for that report. That's ~60 calls per frame
  that take the shared lock for no result.
- **The fix, `patches/bos-03-no-per-frame-event-timing.patch`:** the timing-only events are
  recorded, and the report runs, only with `VLOG(1)` on or `1` in `/tmp/spectrum-971-event-timing`
  (re-read every 2 s, for A/B tests). The 7 events that are real sync points are unchanged.
- **Measured live, 4 cameras, alternating 3 times, 10 s each:**

  | | Detect | PhotonVision CPU | Wakeups | GPU |
  |---|---|---|---|---|
  | Before (timing on) | 3.08 ms | 1.84 cores | 13,700/s | 36.5% |
  | **`bos-03`** | **2.19 ms** | **1.48** | 12,000/s | 34.8% |
  | `bos-03` + GPU lock | 2.14 ms | 1.33 | 10,250/s | 35.6% |

  The same ~0.9 ms came off with 3 cameras (2.77 → 1.89 ms).
- **GPU lock** (`detector/GpuDetectorJNI.cc`, `SPECTRUM_971_GPU_LOCK=1` or `1` in
  `/tmp/spectrum-971-gpu-lock`, re-read every 2 s; **off by default**): one camera's `Detect()` at a
  time, so the cameras queue once per frame instead of on every CUDA call. With `bos-03` it saves
  another ~0.15 cores and costs no latency; before `bos-03` it saved 0.3 cores but added 0.3 ms.
  The stats line shows `gpu lock wait` when it's on.
- **The GPU load is real work.** `nsys profile --trace=cuda` (8 s, 3,866 frames, with `bos-03`):
  0.65 ms of kernels and 0.13 ms of copies and memsets per frame, 31.6 kernel launches and ~58 CUDA
  calls in all. 0.78 ms × 483 fps ≈ 37%, what GR3D shows. Two kernels are 42% of it, and both work
  on the whole image whatever the scene: the first `cub::DeviceSelect::If` (149 µs, compacting the
  mostly-empty `BlobDiff` output) and `BlobDiff` (121 µs).
- **Restart-to-restart noise is at least partly the scene.** After the cameras were replugged, the
  same code without `bos-03` gave 2.8 ms and 1.84 cores instead of 4.4 ms and 2.2. The detector's
  work after `BlobDiff` depends on how many candidate blobs each camera sees, so where the cameras
  point matters. Compare configurations in one run, alternating, as above.
- **Also seen:** TopRight came up at 320x240 after one of the restarts (`health-check.sh` FAIL, one
  of 4 detectors missing; see `photonvision-27`); another restart fixed it. Check that all 4
  detectors report before trusting a measurement.
- **Batching the 4 cameras into one pass was ruled out:** they free-run, so a batch waits up to
  8 ms for the slowest frame.

#### Fused `BlobDiff` (`bos-04`)

- **What:** `BlobDiffCompact` writes only the boundary points, straight into the compacted
  array: each block counts its points in shared memory and takes one global `atomicAdd`. It
  replaces `BlobDiff` writing all 4 slots of every pixel (~1M points, 8 MB at 1280x800) plus
  `cub::DeviceSelect::If` reading them all back. `0` in `/tmp/spectrum-971-fused-blobdiff`
  (re-read every 2 s) goes back to the original pair; on by default.
- **Order:** the points arrive in a different order. The first sort only orders by blob pair
  (bits 24–63), and nothing after it depends on the order within a pair except the angle sort of
  the selected points. That sort now uses the whole 64-bit key (blob, angle, then the point's own
  bits, which are unique), so the result depends only on which points exist.
- **Checked:** `fieldcal_detect` over the synthetic 4-camera recording and the bench sessions
  0006–0009 and 0011 (~5,000 frames, 3,567 tag detections): the CSVs are **byte-identical** fused,
  unfused, and against the Sep 24 build (`bos-01`/`-02` only).
- **Measured live** (4 cameras, alternating 3 times, 10 s each):

  | | GPU | Detect | PhotonVision CPU |
  |---|---|---|---|
  | **Fused** | **28.1%** | **1.88 ms** | 1.43 cores |
  | Unfused | 35.6% | 2.40 ms | 1.52 |

#### Frames stay on the GPU ("GPU input")

- **What:** with the hardware decoder, each gray frame was decoded on the GPU, copied down into
  PhotonVision's Mat, then copied back up by `Detect()`. `snj_decode_gray_dev`
  (`libspectrumnvjpg.so`) now also leaves the Y plane in a per-camera-thread GPU buffer (a GPU to
  GPU copy; the Mat is filled from it, so both hold the same pixels). `processimage` passes that
  buffer to `Detect(image, image_device)`, which then skips the upload. The CPU copy is still
  needed: tag decoding reads it.
- **Which frame is which:** PhotonVision hands the detector a copy of the decoded Mat
  (`GrayscalePipe`), so the pointers differ, and a 180° rotation would change the pixels in
  place. So the GPU copy is used only on the same thread, for a frame of the same size whose 64
  sampled pixels match the decoded frame's. Every 240 such frames per camera (~2 s), the GPU copy
  is read back and compared in full. Any difference turns it off until PhotonVision restarts
  (`971 GPU input: ... DIFFERS`). `0` in `/tmp/spectrum-971-gpu-input` turns it off live. The
  stats line shows `gpu input N%`; it's 100% on all 4 cameras.
- **Measured live** (alternating 3 times): detect **1.32 ms** against 1.55, PhotonVision CPU 1.26
  cores against 1.34, GPU unchanged (27%: the GPU-to-GPU copy replaced the upload). No
  differences found.

#### Where 4 cameras stand now

| | Detect | PhotonVision CPU | GPU |
|---|---|---|---|
| Start (2026-09-29) | 4.4 ms | 2.2 cores | 40–43% |
| + `bos-03` | 2.2 ms | 1.5 | 35% |
| + `bos-04` | 1.9 ms | 1.4 | 28% |
| + GPU input | 1.3 ms | 1.26 | 27% |
| + first-stage graph (`bos-05`) | 1.2 ms | 1.23 | 25.6% |
| + `bos-06`, `photonvision-37` (no gray copy); 10 clean restarts | **1.23 ms** | **1.14** | **25.7%** |

Board power 9.9 W at the end, against 11.6 W at the start; tj 54.7 °C (fan on the quiet
profile).

**With tags in view** (same build, cameras facing up at the shop's ceiling lights, tags held
above them, every camera seeing every tag on every frame, no streams, 3 × 8 s each):

| Same camera pose | Detect avg (worst) | PhotonVision CPU | GPU | Margins |
|---|---|---|---|---|
| No tag | 1.85 ms | 1.42 cores | 36.4% | |
| 1 tag | 2.0 ms (~2.8; one 6.2 spike) | 1.62 | 39% | 26–37 |
| 2 tags | 2.06 ms (2.5–3.4) | 1.67 | 39.9% | 23–37 |

- **The scene mattered more than the tags.** Facing the ceiling lights, with no tag, cost ~10
  points of GPU and 0.6 ms more than the earlier desk scene: more candidate blobs.
- **Tags were cheap.** The first tag (488 decodes a second over 4 cameras) cost +0.2 cores and
  +0.15 ms; the second +0.05 cores and +0.06 ms. Every camera held 122 fps.
- **Backlit tags** (held under the lights) decoded at margins 5–9 until they were held still and
  out of the glare.

#### CUDA wait mode again, and splitting JPEG decode between hardware and CPU

Same scene as the tag tests (cameras facing the ceiling lights, no tag), 4 cameras.
- **CUDA wait mode** (`/tmp/spectrum-971-cuda-sync`, read when PhotonVision starts). Cycled
  block, spin, yield twice, one restart each, 2 × 10 s after each start settled:

  | Mode | Detect (round 1 / 2) | Typical worst (1 / 2) | CPU |
  |---|---|---|---|
  | `block` (default) | 2.16 / 2.33 ms | 4.35 / 4.54 ms | ~1.65 cores |
  | `spin` | 2.11 / 2.45 ms | 4.57 / 4.91 ms | ~1.65 |
  | `yield` | 2.10 / 1.87 ms | 3.94 / 3.71 ms | ~1.6 |

  `yield` was best in both rounds, but by ~0.2 ms, which is as big as the restart-to-restart
  noise. It needs more alternating rounds before changing the default. `spin` no longer gains
  anything, now that the cameras don't queue on CUDA's lock.
- **Hardware and CPU decode split** (`nvjpg:N` in `/tmp/spectrum-jpeg-decoder` or
  `SPECTRUM_JPEG_DECODER`: the first N cameras, in the order they first decoded, use NVJPG, the
  rest libjpeg-turbo; live). Alternating, 12 s each:

  | | Frame age at result | Detect | Decode NVJPG / turbo | CPU |
  |---|---|---|---|---|
  | **All 4 NVJPG** | **13.5 ms** | **2.2 ms** | 3.0 / – ms | **1.6 cores** |
  | `nvjpg:3` | 14.2 ms | 2.9 ms | 2.9 / 3.1 ms | 1.9 |
  | `nvjpg:2` | 15.0 ms | 3.3 ms | 2.9 / 3.2 ms | 2.25 |

  **Worse, keep all 4 on the hardware.** In this bright scene libjpeg-turbo took 3.1–3.2 ms (2.4 ms
  on the earlier plain scene), and freeing NVJPG gained only 0.1 ms. The CPU-decoded cameras also
  lose GPU input (an upload again), and the decode threads compete with the detection threads
  (cross-core wakeups 12k → 24k a second), so detection got 0.7–1.1 ms slower.
- **New: `frame age at result`** in each `971 stats` line: capture (the first USB packet) to the
  end of detection, per camera. 13.5 ms here ≈ 8.1 (the camera sending the frame) + 3.0 (decode) +
  2.2 (detection); add half the exposure for mid-exposure to result.

#### Detection masks drawn on the stream (`photonvision-40`, `bos-07`)

- **Why:** facing the shop's ceiling lights cost ~10 points of GPU and 0.6 ms before any tag was in
  view (above). On a field, the upper part of some cameras' views is arena lighting and truss.
- **UI:** a **Mask** tab (AprilCudaTag and Object Detection pipelines) sets the mode (Off /
  Ignore inside the boxes / Search only inside the boxes). Its "Draw on the stream" switch lets you
  draw on either dashboard stream: drag on empty space to draw a box, drag a box to move it, drag
  any of its 4 corners to resize (the opposite corner stays put), and press Delete to remove the
  selected box. The table lists the boxes, each with a delete button. The mask is shown faintly on
  both streams whenever it's on.
- **Reusable:** `pv-mask-overlay.vue` (components/common) wraps any stream or image with an SVG in
  image fractions and edits a `DetectionMask` ({mode, boxes: [{x, y, w, h}]}, 0–1, on the image as
  displayed, i.e. after rotation). The setting lives on `AdvancedPipelineSettings`, so any advanced
  pipeline can use it; `VisionModuleChangeSubscriber.setProperty` converts it with Jackson.
- **AprilCudaTag (GPU, `bos-07`):** `GpuDetectorJNI.setMask(handle, mode, boxes[])` (called by
  `AprilTagDetectionCudaPipe.setMask` from the pipeline's parameters, only when the mask changed).
  The library turns it into a keep/ignore image at the detector's half size. `ApplyMask` turns
  ignored pixels into "no contrast" (127) right after thresholding, so no blob, edge or tag is found
  there. It's part of the first-stage graph; switching the mask on or off records the graph again
  (at most once a second per detector; meanwhile the steps run one by one). Moving or resizing boxes
  only copies new bytes into the same GPU buffer. An "only inside" mask with no boxes does nothing.
  Log: `971 detector hN: mask ignoring N box(es)`.
- **Object detection:** detections whose centre is ignored are dropped before PhotonVision's own
  filters.
- **Checked** (`fieldcal_detect --mask`, synthetic TopLeft, 814 tags): ignoring the left half
  removed all 349 tags fully on the left and kept all 433 on the right with **byte-identical
  corners**. The 32 tags crossing the middle were dropped, as expected. "Search only the right half"
  gave the same result. In the UI: drawing, moving, 4-corner resize and Delete all reached the
  detector live, and changes are saved 1 s later (ConfigManager).
- **Two dashboards editing the same mask overwrote each other** while testing: one tab's stale
  boxes replaced the other's. The cause was a general bug, fixed in `photonvision-41` (below).

#### Other dashboards never saw setting changes (`photonvision-41`)

`VisionModule.saveAndBroadcastSelective` sends each pipeline-setting change to the other open
dashboards as `{mutatePipelineSettings}`. But `App.vue` applies it only when the message also
carries `cameraUniqueName`, which it never did. So a second dashboard showed stale values for
every setting (sliders, exposure, masks) until reloaded, and could send them back. Now the message
carries the camera's name. Checked: a change in one tab showed up in a second tab within 2 s, and
a mask box added from one browser appeared live on another.

#### Unplugging a camera, and cold boots, with 4 cameras (2026-09-29)

All on the finished build (patches up to `photonvision-44`, `bos-07`). Logs recorded with
`journalctl -f` and the kernel log.
- **TopRight pulled for ~10 s, then plugged back into the same port:**
  - Frames stopped at the kernel's "USB disconnect". cscore retried opening it every 0.3 s.
  - Plugged back in, the kernel enumerated it in 0.2 s (and the payload cap re-applied).
  - cscore reconnected, restored 1280x800 at 120 fps and its settings, and it was detecting 1.3 s
    after going back in, at the full 122 fps by 2.2 s.
  - The other 3 cameras: minimum 121.0 fps, average 122.0.
- **TopLeft yanked and pushed straight back (out 1.0 s):**
  - The kernel re-enumerated it 0.23 s after it went back in.
  - cscore reconnected 1.35 s after the frames stopped. Its first open raced the kernel finishing
    setup and failed a dequeue, and the second succeeded 0.4 s later.
  - One partial second of detections was lost (37 fps), then 122.
  - The other 3: minimum 119.7 fps.
  - The watchdog never had to act: cscore's own reconnect handles a camera that leaves USB.
- **Two cold power cycles** (power cut ~5 s):

  | | Linux ready | All 4 detecting |
  |---|---|---|
  | Boot 1 | 16.7 s | 21.8 s |
  | Boot 2 | 16.1 s | 21.1 s |

  Both times all 4 cameras came up at 1280x800 and 122 fps, with GPU input 100%, 4 first-stage
  graphs recorded, and no capture errors, detector failures, hardware decoder off, S_FMT EBUSY,
  or `ConcurrentModificationException`. `health-check.sh` said READY. That's the same ~20 s to
  detecting as with 2 cameras, so tonight's startup work (the capture lock, graph recording)
  costs no measurable boot time.

#### Five Thriftiest Cams (2026-09-29)

The 5th camera ("5th Cam") on a USB 3 hub in the Jetson's USB-C port (USB 2.0 side `1-1.1`),
so SSH went over Wi-Fi. `payload_cap` had a leftover `1-1.1:256` port entry, which beats the
model's 1280. It was raised to 1280 at runtime only (`/etc/modprobe.d/90-spectrum-uvcvideo.conf`
unchanged), so after a reboot that port is 256 again unless it's set on the USB bandwidth card.
- **USB budget:** 6,400 of ~6,720 bytes per 125 µs allocated (320 free), 39.6 of 53.8 MB/s used.
  All five held 121–122 fps. The largest frames were 52–76 KB. BottomLeft's 75.5 KB used 88% of its
  allocation ("fits 1.1x"), and three cameras were at 80–88%. At 90% or more a camera compresses
  harder instead of dropping frames. A 6th camera doesn't fit at this cap.
- **Load** (cameras facing the ceiling lights, no tag, 3 × 10 s), against 4 cameras in the same
  scene. The first 5-camera run still had 2 streams "open": dead connections from the laptop's
  unplugged USB-C link (below). The clean run was after a PhotonVision restart, with none:

  | | 4 cameras | 5 cameras, 2 dead streams | **5 cameras, clean** |
  |---|---|---|---|
  | Frames/s | 488 | 610 | 610 |
  | Detect avg (worst) | 1.85 ms | 2.4 ms (3.7–5.9) | **2.1 ms** |
  | GPU | 36.4% | 45.5% | **44.4%** |
  | PhotonVision CPU | 1.42 cores | 1.9 cores | **1.87 cores** |
  | Frame age at result | 13.5 ms | 14.0 ms | **13.3 ms** |
  | NVJPG decode | ~3.0 ms | 3.3 ms | **2.8 ms** |
  | Board power, tj | ~10.6 W | 11.2 W, 56.8 °C | **11.1 W, 56.5 °C** |

- **Scaling is linear now:** GPU +25% for +25% frames. The next limits are USB (full) and the two
  NVJPG engines (queueing more), not the GPU or CPU.
- **The hub** (with a built-in ASIX AX88179 gigabit Ethernet) logged register errors (`Failed to
  write reg ... -32`) the whole time. That's harmless for the cameras, but it floods the kernel
  log; use a plain USB 2.0 hub on the robot.
- **Fanless:** 11.1 W is close to the 12 W fanless test (~73 °C with the plate), so test fanless
  with 5 cameras before relying on it.
- **Dead viewers kept their streams for ~15 minutes.** The laptop's USB-C link was unplugged with
  a dashboard open, and its two stream connections stayed "established" with ~78 KB queued each.
  cscore still counted them as viewers, so PhotonVision kept resizing and encoding those frames
  for nobody. Linux gives up on an unanswering peer only after `tcp_retries2` retransmissions
  (default 15, ~15 min with data queued). `ss -K` can't close them on this kernel. That happens
  whenever a laptop leaves without closing the dashboard: lid shut, cable pulled, Wi-Fi gone,
  driver station swapped.
- **Fix: `09-robot-tuning.sh` step 11**, `net.ipv4.tcp_retries2 = 5` in
  `/etc/sysctl.d/90-spectrum-tcp.conf` (gives up after 0.2 s × (2⁶ − 1) ≈ 12.6 s; `--undo` restores
  15). It applies to every connection: NT reconnects by itself, and an SSH session dies if the
  network is out for over ~13 s with data waiting. **Tested:** a dashboard open over Wi-Fi, then
  the laptop's Wi-Fi switched off. The Jetson dropped its two stream connections 11.4 s and 12.9 s
  later (clocks lined up from NetworkManager's log and `date` on both). The Wi-Fi came back after
  9 s, but the drops came 2–4 s after the radio did, before the laptop could have rejoined.

#### Upstream fixes and a real reconnect for stuck cameras (`photonvision-42` to `-44`)

From the upstream review (`docs/UPSTREAM-PORT.md`, 2026-09-29):
- **`photonvision-42` (upstream #2617):** `currentVideoFormat` is `VideoFormat | undefined`, and its
  callers allow for it, as do the stream's aspect ratio and the 3D view (they indexed
  `validVideoFormats` directly). Before, dashboard tabs could vanish while a camera was activating.
- **`photonvision-43`: the stuck-camera "reconnect" now reopens the camera.** `photonvision-29`'s
  first step set `kForceClose` and then `kAutoManage`, which in cscore's Linux camera only stops and
  restarts streaming (see `photonvision-37`). So in its bench test the camera came back only at the
  USB-reset step. `reconnectCamera` now switches through another video mode and back: cscore then
  closes the device, opens it, sets the format and streams again, twice. The video-mode fix
  (`-37`) and the bandwidth retry share it. Tested with the stuck-camera hook on BottomLeft:
  "reopening it (switching through 640x360)" at +3.0 s, cscore `set format 640x360` then
  `1280x800`, hook lifted at +4.5 s, and "delivering frames again, after a reconnect" with no USB
  reset. All 4 cameras at 122 fps afterwards.
- **`photonvision-44` (upstream #2352):**
  - `reactivateDisabledCameraConfig` logged "already in use by active VisionModule! Cannot
    reactivate", then reactivated anyway, binding two modules to one device. Now it puts the config
    back and returns false, as the "add camera" branch next to it already did.
  - Camera Matching cards are titled by nickname, with the device model underneath; identical
    cameras were all "Thrifty:".
  - The delete dialog names the camera being deleted, not the one selected on the dashboard.
  - Not tested live: there were no disabled configs left to try it with.

#### Start from, Create on every camera, Switch all (`photonvision-46`)

- **`POST /api/settings/createPipeline`** `{name, type | fromCamera + fromPipeline, cameras, switchCamera}`
  answers `{created: [{camera, index}], skipped: [{camera, reason}]}`. The new-pipeline dialog uses
  it for every create, blank or not. `VisionModule.createPipeline` does the work:
  - adds a pipeline of the source's type (or `type`) through `PipelineManager.addPipeline`, so it
    starts from this camera's defaults;
  - copies every public field from a deep copy of the source (JSON round trip: `clone()` is
    shallow and would share the mask, HSV ranges and offset points), except the index and name;
  - from another camera, also skips `inputImageRotationMode`, the exposure limits and
    `detectionMask`, and `cameraVideoModeIndex` unless the video mode lists are equal (the same
    rule as Copy settings, `-25`);
  - skips a camera that already has a pipeline by that name. Only `switchCamera` switches to it.
- **Switch all** is client-only: `changeCurrentPipelineIndex(N, true, camera)` for each camera with a
  pipeline N that isn't in driver mode (-1), calibrating (-2) or focusing (-3). The snackbar names
  the cameras skipped and any whose pipeline N has a different name.
- **`uiState`** now also has `pipelines`: every pipeline's saved settings (unwrapped from Jackson's
  `["type", {...}]`), so tests can check pipelines that aren't running. A pipeline deleted during
  the read ends the list rather than throwing (the first version threw a NullPointerException).
- **Tests** (`tests/ui/specs/pipelines.spec.ts`):
  - A same-camera copy has every saved setting equal to the source.
  - Create on every camera from TopLeft's `zz-uitest` (Decision Margin 23, 90° orientation) gave
    "Created 'zz-uitest-all' as pipeline 2 on TopRight, TopLeft; as pipeline 1 on 5th Cam,
    BottomRight, BottomLeft". Every copy has 23; only TopLeft's has 90°; the others stayed on their
    pipelines.
  - Switch all from pipeline 2: "switched TopRight. Not switched: 5th Cam (no pipeline 2),
    BottomRight (no pipeline 2), BottomLeft (no pipeline 2)".
  - The test then deletes the copies everywhere and puts every camera back on its pipeline.
  - A test guard: an entry must carry the pipeline's name. Before the unwrap fix, two error
    entries compared equal and the exact-copy test passed without checking anything.
  - A slider sends 20 ms after the last change, so the test waits for the backend before switching
    tabs. A tab closed within those 20 ms drops the change; a person can't switch that fast.

#### Pipeline numbers shown (`photonvision-47`)

The dashboard's Pipeline dropdown and the Camera Matching page's pipeline lists show "N: name"
(Copy settings and Start from already did). Profiles and Switch all go by number, and names alone
hid mismatches such as TopRight's pipeline 1 being "Fuel Test". Test: the dropdown's options equal
`uiState`'s `pipelineNicknames` numbered from 0. The test helpers read and pick pipelines by name
inside "N: name". Suite: 5 tests, 1.5 min, all passing.

#### Frame timestamps from the camera clock (`uvcvideo hwtimestamps=1`, 2026-09-30)

- **Before:** uvcvideo stamps a frame when its first USB packet is processed. It processes packets
  in URBs of 32 microframes (4 ms), so stamps land on 4 ms steps: intervals of 8 or 12 ms instead
  of 8.245, jitter 0.95 ms, identical on every camera. (The camera does mark each frame's end,
  "Frame complete (EOF found)" in the driver trace, so there's no wait for the next frame.)
- **After:** `hwtimestamps=1` makes uvcvideo convert the camera's own PTS (in its clock, through
  the SCR it sends) to the host clock. The Thriftiest Cam sends both. On the stamp's meaning: it's
  0.74 ms before the first-packet stamp on average, so it marks roughly when the camera starts
  sending, not the start of exposure. `photonvision-13`'s half-exposure correction is unchanged,
  and the camera's delay from the end of exposure to sending is still unmeasured
  (`SPECTRUM_CAMERA_DELAY_US`, the robot spin test).
- **Measured** (`tests/uvc-timestamps/run.sh`: v4l2-ctl, PhotonVision stopped, one or all
  cameras):

  | 600 frames per camera, all 5 at once | Jitter | Intervals |
  |---|---|---|
  | first USB packet | 0.95 ms | 7.97-12.00 ms |
  | camera clock | 0.004-0.007 ms | 8.22-8.27 ms |

  **As the robot sees it** (`tests/fake-robot/timestamps.sh`: a fake robot, enabled, subscribed to
  every camera's results; the capture timestamp in each result's metadata; 30 s, 3,640 results a
  camera): jitter **0.955 -> 0.008-0.011 ms** (BottomLeft 0.036, range 7.60-8.89 ms, from the
  occasional clock-recovery correction). 1 ms of timestamp error is 0.36 deg at 360 deg/s.
- **Where:** `/etc/modprobe.d/91-spectrum-uvcvideo-timestamps.conf`, written by
  `11-uvcvideo-payload-cap.sh --install` (its own file, so the Camera Matching page's
  `payload_cap` rewrites never touch it). The parameter is also writable at run time; streams
  opened after the change use it (restart PhotonVision). `health-check.sh` checks it. Frame age
  in the `971 stats` lines reads 0.5-1 ms higher, as the stamps are earlier and truer.
- The fake robot result reader needed PhotonVision's own type string (`photonstruct:...`) to
  subscribe, and a topics-only subscription for the topics to be announced to it at all.

#### Smaller USB batches: 16 packets per URB (`kernel/uvcvideo-urb-packets.patch`)

uvcvideo queues 5 isochronous URBs of `UVC_MAX_PACKETS` (32) packets, 4 ms each at one packet per
125 us microframe, and sees a URB's packets only when the whole URB completes. So a frame's last
packet (the camera marks the end with EOF) can wait up to 4 ms, 2 ms on average, before the frame
is handed over. The patch adds `urb_packets` (0 = stock); like `payload_cap` it's writable at run
time and applies when a stream next starts. `tests/`: `urbtest.sh`-style runs, 40 s warm-up then
60 s per value, 5 cameras, camera-clock timestamps:

| `urb_packets` | Frame age at decode | At result | Whole-system CPU | Interrupts/s |
|---|---|---|---|---|
| 32 (stock), 3 runs | 9.33 ms | 12.98 ms | 1.09 cores (0.98-1.26) | 15,500 |
| **16**, 3 runs | 8.25 ms | **11.76 ms** | 1.18 cores (1.16-1.20) | 17,900 |
| 8, 2 runs | 7.94 ms | 11.61 ms | 1.30 cores (1.19-1.40) | 20,000 |

- **16 kept:** 1.2 ms lower latency at the result, every run, worst frame age 17 ms instead of
  18-21 ms, 122.2 fps, no USB errors, about 0.1 core more (within the stock runs' spread).
  PhotonVision's own CPU is unchanged; the extra is kernel time.
- 8 gains only 0.15 ms more for more CPU and interrupts: the host controller doesn't interrupt
  much more often than every millisecond.
- The remaining ~8 ms from first packet to decode is the camera: it paces a 24 KB frame out over
  about one frame period while its sensor reads out (the frame takes ~2.4 ms at our 1280-byte cap
  if sent at once).
- **Live vs saved:** `health-check.sh` now compares each driver setting's live value with
  `/etc/modprobe.d` and warns when a reboot would change it. Reinstalling the driver showed why:
  the 5th camera's 1280-byte cap (port 1-1.1) had only been set live, so the saved file still had
  256, and the Camera Matching card showed 1280 with nothing to save. Saved on 2026-09-30.
- Saved in `/etc/modprobe.d/92-spectrum-uvcvideo-urb-packets.conf` by
  `11-uvcvideo-payload-cap.sh --install` (`URB_PACKETS` overrides); `--undo` removes it with the
  patched driver. `health-check.sh` checks it.

#### Garbage collection and the worst-case detects (2026-09-30)

The 4-7 ms worst-case detects seen earlier came with dashboards streaming or tests running; a clean
2-minute run had none over 1.8 ms. Garbage collection does cause the remaining ones: with the
default setting (G1, `-Xmx512m`), 3 of the 4 seconds with a detect over 3 ms contained a young
collection, against 12% of seconds overall. `tests/jvm-gc/probe.sh` switches the GC log on live
(`jcmd VM.log`, no restart) next to the per-second worst detects; `compare.sh` restarts with other
flags. 5 cameras, ceiling scene:

| JVM | Collections | Pause | Seconds with a detect over 3 ms | Worst | CPU | RSS |
|---|---|---|---|---|---|---|
| **G1 `-Xmx512m` (kept)** | every ~8 s | 6-10 ms | 4 in 2 min | 4.9 ms | 0.98 | 2.29 GB |
| G1 `-Xms1g -Xmx1g -Xmn512m` | every ~3 min | **274-278 ms** | 3 in 8 min | 30.6 ms | 0.97 | 2.96 GB |
| same + `-XX:+AlwaysPreTouch` | every ~3 min | **276-287 ms** | 7 in 6 min | 7.8 ms | 0.95 | 3.33 GB |
| ZGC `-Xmx1g` | concurrent | < 0.2 ms | 21 in 6 min | 22.5 ms | 1.07 | 2.74 GB |

- **Kept the default.** A 512 MB young generation turns the 7 ms pauses every 8 s into a
  quarter-second freeze of every camera every 3 minutes. The first 2-minute test of it ended before
  its young generation filled and looked perfect; the longer run showed the pause. Pre-touching the
  heap didn't change it, so it isn't page faults: G1 copies more survivors from a young generation
  that lives 3 minutes instead of 8 seconds. ZGC's pauses are tiny, but its concurrent work and
  barriers gave more slow seconds and 0.1 core more.
- **Averages don't move.** A collection delays about one frame per camera in ~976, by up to ~7 ms:
  about 0.007 ms on the average latency. Frame rates stay at 122. Results keep their capture
  timestamps, so the pose estimator places a late one correctly; only its arrival is late.
- The GC on disable (`photonvision-49`, 27 ms while disabled) stays.
- PhotonVision allocates about 2.6 MB/s (young 37 MB -> 19 MB every ~7 s).

#### Far-tag search (`detector/far_search.cc`, `photonvision-55`)

bos hard-codes `quad_decimate` 2 (quads found on a 640x400 image; refinement and decoding still use
the full image). Measured 2026-09-24: ~20 px is the smallest tag found reliably, against ~12 px on
the full-size image, which costs ~2.75x the GPU every frame. This searches the full-size image only
when the robot is short of a good pose:

- **Policy** (all cameras share it: they're one process). A camera has a *good view* with at least
  2 tags of 40 px or more and decision margin 30 or more (`GoodView`; near tags, a solid multi-tag
  pose; judged from the detections, not PhotonVision's multi-tag, which needs a calibration three
  cameras don't have yet). No good view on any camera for 250 ms = *starved*: then one camera at a
  time (round robin: the active camera whose last full-size search is oldest), at most
  `farSweepsPerSecond` (30) across all, runs a **full-size search**: its frame upscaled 2x
  (nearest neighbour, plain C++), through one shared 2560x1600 971 detector, so the half-size
  search sees every pixel. Tags it finds that the normal search didn't are **tracked** with
  160x160 crops upscaled to 320x320 (a detector per camera), one crop per camera frame, until
  they're 24 px or more in the normal search's results or unseen for 300 ms. The moment any
  camera has a good view, no searches or crops, and the tracks are dropped.
- **Guards:** after a full-size search taking T, the next waits at least 5 T (at most ~20% of the
  time, however slow the scene makes them). The shared detector is built on the first frame
  (~200 ms), not on the first search. Far-search detectors have their own tag family: adding a
  family to a detector stores that detector's decode table in it, so sharing the camera's would
  overwrite and on destroy free the camera detector's table. The detection mask is applied to
  far tags by their centre. No OpenCV functions (PhotonVision's JVM has its own OpenCV; a second
  one's symbols could clash): the upscale and crop are plain loops.
- **Coordinates:** X = U/2 in the upscaled image. The AprilTag library's decimation convention
  suggested X = U/2 + 0.25; `far_search_test` measured the full-size corners +0.24 px off with that
  (the 971 detector puts pixel edges at whole numbers), and -0.01 / -0.02 px (worst 0.125) with 0.
  The homography is scaled with it (H' = T H) and the crops' camera matrix follows each crop.
- **Bench test** (`tests/far-search/run.sh`, `detector/far_search_test.cc`, on the Jetson; real
  tag36h11 tags from `apriltag_to_image`, 7 deg, blurred, camera-like noise):

  | | Result |
  |---|---|
  | Smallest tag found 4 of 4 times | normal 18 px, far search **10 px (1.8x)** |
  | Corners, full-size against normal | -0.010 / -0.017 px mean, 0.125 px worst |
  | 2 near 70 px tags + a far 14 px one, 1 s | 0 full-size searches, 0 extra tags |
  | Near tags hidden | starved after 246 ms; far tag found then, and on 92 of 92 frames after as it moved 1.5 px a frame (the normal search alone: 92 of 122) |
  | Near tags back | 0 extra tags at once |
  | Budget | 21 full-size searches in the 0.75 s starved (30 a second) |
  | Off | nothing extra |

  The first version of the test used raw per-pixel noise: through the upscale every speck was a
  candidate quad and a search took 94 ms. Camera-like noise (smoothed, as JPEG does) takes 4-6 ms,
  as the real recordings measured before.
- **Live** (5 cameras, bench, no tags in view, so searching the whole time; 60 s each):

  | | Off | On |
  |---|---|---|
  | fps | 122 all | 122 all |
  | GPU | 16.5% | 19.4% |
  | Board | 9.7 W | 10.1 W |
  | Frame age at result | 12.08 ms | 12.29 ms |
  | Worst detect per second | ~1.0 ms | ~2.0 ms (sharing the GPU with a search) |

  27 full-size searches a second, 2.7 ms each; `971 far search 10 s:` lines in the log. While any
  camera has a good view it's off, so this is the most it costs.
- **Settings:** `farSearch` (default on) and `farSweepsPerSecond` in `spectrum-robot-state.json`,
  Settings > Robot state; `GpuDetectorJNI.setFarSearch` / `farSearchStatus` (retried at most every
  5 s until the library is loaded). `/api/robotState` has the counters; `health-check.sh` reports
  it. Browser test `far-search.spec.ts`: the status, and no searches while off.
- **`bos-08`, found by replaying recordings:** one frame of session 0008 (TopRight 174, a busy
  bench scene) made a full-size search run for minutes (still going at 80 s; `fieldcal_detect
  --upscale 2`, which doesn't use the far search, hung on it too). In `RefineEdges`,
  `nsamples = max(16, edge length / 8)` is unbounded: a candidate quad with a corner far outside
  the image gave hundreds of millions of samples, each searched over 25 steps. The half-size
  search never makes such quads (they stay inside its 640x400 image), so it never showed before.
  The patch skips quads with a corner that isn't finite or is more than an image's size outside
  it, and caps `nsamples` at (width + height) / 8 (260 for 1280x800; only edges over 2,000 px
  reach it). Frame 174 at full size now takes a few ms; normal-size results are unchanged (0007:
  305 sightings before and after; bench test passes). Live, one such frame would have frozen a
  camera's thread: the time guard spaces searches out but can't stop one that has started.
- **Replay: `far_replay`** (`detector/far_replay.cc`, `tests/far-search/replay.sh`). A whole Rewind
  session, every camera merged in recording-time order, through per-camera detectors with
  PhotonVision's settings and the far search deciding on each frame on the recording's clock (its
  policy spans cameras, so they replay together). `--off` for a baseline, `--calib CAMERA=...`,
  `--budget`, `--every`, `--trace` (each frame's far-search time as it goes, to find a stall),
  `--out` (every detection with its source, normal or far). It reports tag sightings with and
  without the far search, tags only the far search found and their smallest size, time starved,
  searches and crops, and the slowest frames. The recording reader and JPEG decoder moved to
  `detector/rewind_reader.h`, shared with `fieldcal_detect`.
  - **Known-answer check** (`replay.sh`): `far_search_test --write-session` writes a synthetic
    session (CamA: near tags for the first and last second, a moving 14 px tag all along; CamB:
    none; JPEG quality 85). `far_replay` finds the far tag on 14 frames the normal search missed,
    all inside the no-good-view window (1.25-2.0 s), and nothing extra with `--off`.
  - **The five bench recordings** (2026-09-24, no far tags in them): all replay in 1-8 s; full-size
    searches 4.5-10 ms each (the replay shares the GPU with the live cameras); the only slow frame
    in each is the first (the detector build, ~130 ms).
  - **Deadlines:** `replay.sh` gives each session 60 s + 1 s per 100 frames and reports TIMEOUT as
    a failure. The first run used a flat 15 min per session, which hid the stalled frame for 6 min.
- **Not tested yet:** real far tags (on a field, or a long hallway). The library before the far
  search is at `/usr/lib/lib971apriltag.so.before-far-search` on the Jetson.

#### Tag contrast and the Gain slider (`photonvision-54`)

- **`TagContrast`:** for each detection the GPU AprilTag pipeline keeps (after the decision margin
  and hamming filters) that's at least 24 px a side, a homography from the unit square to its
  corners places 64 samples half a cell inside the edge (the black border; 36h11 is 8 cells across
  it) and 64 half a cell outside (the 1-cell white margin), along the middle 70% of each side.
  Medians of white and black, and the share of white samples at 250 or more. At most every 200 ms
  per pipeline; the measurement rides on `CVPipelineResult`, and `VisionModule` records it per
  camera. `GET /api/tagContrast` gives each camera's medians over the last 2 s and a verdict:
  whites clipping (over 10% at 250+), low contrast (under 50), blacks lifted (over 80), good, or no
  tags (none measured in 3 s). Thresholds are first guesses to check against a tuner run.
- **Shown:** the Input tab (above Auto Exposure, polled every 1 s), Match Ready's tiles, and
  `health-check.sh` (PASS or WARN, only for cameras with a tag in view).
- **Unit tests** (`TagContrastTest`, photon-core, on the laptop): a flat-on tag of 30 on a 200
  margin reads white 200 / black 30 / 0% clipped; a tilted one on a 255 margin reads 100% clipped;
  a 20 px tag and a measurement 0 s after the last return nothing; the verdicts and advice. 4 of 4.
- **Gain:** `QuirkyCamera` lists the Thrifty OV9281 (1bcf:28c5) with `Gain` (upstream #2478), but
  ours has no gain control (`v4l2-ctl -l`: brightness -64..64 at 64, contrast, gamma 176 against a
  default of 150, sharpness, backlight compensation, exposure; no gain). `hasGainControl()` now also
  asks the camera (`VisionSourceSettables.hasControl`, true until it's connected so a camera with
  real gain keeps its value while unplugged), so `cameraGain` is -1 and the slider hides, and the
  Field Calibration tuner no longer steps gain. Browser test: `camera-controls.spec.ts`.
- **Not measured yet:** brightness at +64 (the maximum, chosen in the tuning guide to make up for no
  gain) adds an offset that lifts the tag's blacks too; gamma 176 isn't the default. The Field
  Calibration tuner with a tag in view settles both; the tag contrast readout will say why.

#### Settings snapshots (`photonvision-53`)

- `SettingsSnapshots` keeps `/opt/photonvision/snapshots/ID/` (ID = `yyyyMMdd-HHmmss`):
  `photon.sqlite` copied with SQLite's `VACUUM INTO` after `saveToDisk()` (consistent even if a save
  is running), the config folder's `spectrum/` (excluded tags), `extra/` (Robot state, Rewind
  settings) and `meta.json` (name, reason, time, version, each camera's pipeline names). 2.0 MB.
  The whole config folder is 76 MB, but 31 MB is logs and 35 MB calibration images.
- **Restore** takes a "Before restoring 'NAME'" snapshot, stops the write task and the flush on
  exit, replaces `photon.sqlite` (and removes any `-journal`/`-wal`/`-shm`), `spectrum/` and the
  extra files, then restarts PhotonVision. Unlike the stock settings import it doesn't delete the
  config folder, so logs and calibration images stay.
- **Automatic:** "before restore", and "Field connected DATE" the first time the FMS attaches
  each day (off the NetworkTables thread). The newest 20 automatic ones are kept; named ones stay
  until deleted.
- `GET/POST /api/snapshots`, `POST /api/snapshots/restore {id}`, `POST /api/snapshots/delete {id}`,
  `GET /api/snapshots/download?id=` (a zip). The card reloads its list when the page reconnects
  after a restore (the first version showed the old list until reopened).
- Test (`tests/ui/specs/snapshots.spec.ts`): save "zz-uitest snapshot" in the card, add pipeline
  `zz-uitest-snap` on TopLeft, restore through the card's confirm dialog. PhotonVision restarted
  and came back in ~10 s with every camera's pipelines and running pipeline as before and no
  `zz-uitest-snap`; the "Before restoring" snapshot lists it. Both are deleted afterwards. The
  Event test deletes the "field connected" snapshot its fake field causes, and the run fails if a
  `zz-uitest` snapshot is left.
- A manual copy of the settings from before the first restore test is at
  `~/photon.sqlite.before-snapshots-20260930-080654` on the Jetson.

#### Match Ready page (`photonvision-52`)

- `GET /api/healthCheck` runs `health-check.sh` (through `/opt/photonvision/health-check.sh`, a
  link to the repo's copy made by `06-install-fork-jar.sh`; `SPECTRUM_HEALTH_CHECK` overrides)
  under `timeout 60`, one run at a time, reusing a run less than 3 s old. It parses the script's
  own output (`== Section`, `PASS/WARN/FAIL text`, other lines as info, the final `READY` /
  `NOT READY` line), so the page and SSH always agree. 3.7 s as root. `health-check.sh` finds
  `usb-bandwidth.py` through `readlink -f`, so it works through the link.
- The page (`/#/ready`, "Match Ready" in the sidebar): the verdict, the WARN and FAIL lines first,
  a tile per camera from the results the dashboard already receives (pipeline "N: name" and type,
  fps against the mode's rate, latency, targets, calibrated at the current resolution for AprilTag
  and ArUco pipelines; low fps isn't flagged while idling), the robot line from `/api/robotState`,
  and every section folded. Re-runs every 30 s while open.
- Test (`tests/ui/specs/ready.spec.ts`): the verdict matches the endpoint's, the sections include
  PhotonVision / Cameras / Robot connection / System, each camera's tile shows its pipeline and a
  non-zero fps, and Check again brings a newer run. Suite: 9 tests, 2.3 min.

#### Event pipeline when the field connects (`photonvision-51`)

- Settings `eventProfileOnFms` (default off) and `eventPipeline` join idle mode's in
  `spectrum-robot-state.json`. On the control word's FMS-attached bit going false to true (this
  includes connecting to a robot that's already on the field), `IdleMode.switchAllTo` sets every
  camera that has that number to it, skipping driver mode (-1), calibration (-2) and focus (-3).
  Only on that edge, so later switches (dashboard, robot code, the setting turned off) stick.
- The summary, kept as `lastEventSwitch` and logged, names cameras whose pipeline at that number
  isn't the one most cameras have there: "the field (FMS) connected, pipeline 1: switched TopRight
  (its 1 is 'Fuel Test'), 5th Cam, BottomRight, TopLeft, BottomLeft".
- `POST /api/robotState {switchNow: true}` runs it by hand (Settings' Switch now).
- `health-check.sh`: PASS with each camera's pipeline at that number, WARN if one lacks it or
  has a different name there.
- Test (`tests/ui/specs/event.spec.ts`, 37 s): `zz-uitest-all` on every camera (pipeline 1 on
  four, 2 on TopRight), turned on and chosen in the Settings card. The fake robot runs disabled 4 s,
  then FMS-attached. Every camera switched to 1, and the summary named TopRight's Fuel Test. The
  test then switched TopLeft back on the dashboard, and 5 s later (FMS still attached) it hadn't
  moved. Switch now moved it again. Afterwards the settings are restored, the pipelines deleted,
  and every camera put back. Suite: 8 tests, 2.0 min.

#### Idle mode switch and Robot state card (`photonvision-50`)

- `IdleMode` settings `{idleWhileDisabled, idleFps}` live in
  `/opt/photonvision/spectrum-robot-state.json` (`SPECTRUM_IDLE_FPS` is only the default).
  `GET/POST /api/robotState` returns them with `robotConnected`, `enabled`, `autonomous`,
  `fmsAttached` and `idleNow`. The GC-on-disable listener is registered even with idle off.
- Settings > **Robot state**: what PhotonVision sees of the robot, the switch, and the idle rate
  (5-60 fps). Dashboard: while idling, "Robot disabled: cameras idle at 30 fps, full speed on
  enable" under the FPS, with a **Full speed** button. Both poll `/api/robotState` (1 s / 2 s).
- `health-check.sh`: PASS "idle while disabled: 30 fps per camera", or WARN while it's off.
- Test (`tests/ui/specs/idle.spec.ts`): starts `tests/fake-robot` over SSH (50 s disabled, ended
  early with `/tmp/fake-robot-stop` so later tests don't see a robot). With the robot disabled, the
  notice shows and the FPS reads under 40 (29). Full speed turns idle off and the FPS goes over
  100. The Settings switch turns it back on. The setting is restored afterwards. 9 s. Suite: 7
  tests, 1.5 min.

#### Idle while disabled, GC on disable (`photonvision-49`)

- **`IdleMode`:** `active()` is true while NetworkTables is connected and the Driver Station's
  control word (`/FMSInfo/FMSControlData`, already read by upstream's `NTDriverStation`, which only
  logged it) says disabled. `VisionRunner` then waits before each grab until 1/`SPECTRUM_IDLE_FPS`
  (default 30) after the last one, in sleeps of at most 5 ms that end as soon as the robot is
  enabled. Frames not grabbed are never decoded, so both the NVJPG decode and the detector are
  saved. `NTDriverStation` now keeps `current()` and calls transition listeners.
- **GC:** 0.5 s after each enabled-to-disabled transition, `System.gc()` on a daemon thread, logged
  as "Robot disabled: garbage collected in N ms, heap A -> B MB".
- **Test** (`tests/fake-robot/run.sh`, on the Jetson): PhotonVision looks for team 8515's robot at
  10.85.15.2, so the script adds that address to `lo` for the run and `FakeRobot.java` (a
  NetworkTables server using PhotonVision's own jar) answers there, with no settings change. It
  publishes the control word through phases and prints each phase's fps per camera and board
  power. 5 cameras, ceiling scene:

  | Phase (30 s) | fps per camera | Board |
  |---|---|---|
  | disabled | 30-31 | 7.8 W |
  | enabled | 122 | 9.7 W |
  | disabled | 31 | 7.8 W |

  "Robot disabled: garbage collected in 27 ms, heap 24 -> 11 MB" 0.5 s after the second disable.
- **Not yet checked on 2027 robot code:** that 2027 WPILib still publishes `/FMSInfo/FMSControlData`
  to coprocessors. If it doesn't, idle mode never engages (it fails safe: full rate).
- **Deadlines:** the script re-runs itself under `timeout` (the phases plus 60 s), so cleanup still
  runs on a timeout; `FakeRobot` gives up after 30 s without a connection. `tegrastats` is stopped
  with `tegrastats --stop`: killing its `sudo` left it running and holding the script's output
  open, which hung the first run.

#### Hidden tabs close their streams (`photonvision-48`)

- `photon-camera-stream` sets its `img` to the empty source while `document.visibilityState` is
  `hidden`, which closes the MJPEG connection; `photonvision-15` then stops encoding that stream.
  Shown again, it reconnects.
- **Upstream bug found on the way:** the stream URL called `inject("backendHostname")` inside a
  `computed`. `inject` only works during setup, so any recompute (tab shown again, or the backend
  reconnecting after a PhotonVision restart) built `http://undefined:PORT/stream.mjpg`. Now read
  once at setup.
- `uiState` has `streamViewers` per camera (cscore's source enabled = a client is streaming).
  Test (`tests/ui/specs/streams.spec.ts`): the dashboard's stream reaches the Jetson; hidden (the
  browser's visibility state and event), the Jetson has no viewer within 10 s; shown, the viewer
  and a decoded frame are back. Skipped if another dashboard is already watching that camera.

#### Browser tests (`tests/ui`, `photonvision-45`)

Playwright, run from the laptop in its own Chrome against the live Jetson (`tests/ui/run.sh`, which
opens the SSH tunnel for port 5800 and the stream ports 1181-1200). Written after the gamma slider
(`-39`) and the second-dashboard bug (`-41`) got through: in both, the camera changed but a page
didn't show it.
- **`photonvision-45`:** the shared controls (`pv-slider`, `pv-switch`, `pv-select`,
  `pv-range-slider`, `pv-number-input`, `pv-radio`, `pv-input`) carry `data-pv-control` and
  `data-pv-label` (sliders also `data-pv-min/max/step`), so tests find controls by label rather than
  by Vuetify's generated ids. `GET /api/spectrum/uiState` returns the backend's own copy of each
  camera's current pipeline settings and extra controls. Read-only.
- **Nothing is reset.** Each test duplicates the camera's current pipeline, renames the copy
  `zz-uitest`, works on it, then deletes it and switches back (the fixture in `lib/fixtures.ts`).
  A killed run's leftover `zz-uitest` is deleted by the next one. Refuses to run while the robot is
  connected (`/api/rewind`'s `robotConnected`).
- **`round-trip`:** every visible control on every tab. It changes each one the way a person would
  (the arrow buttons for sliders, the menu for selects), then checks that the page shows the value,
  that the backend changed (a diff of `uiState`, which also names the setting each control
  moves), and that a second browser context shows it. Then it puts the value back and checks all
  three again. On TopLeft's AprilTagCuda copy: 20 controls in 30 s. The whole suite (4 tests) takes 1.2 min.
  - Extra controls store -1 for "camera default", so -1 is compared as the default.
  - "Draw on the stream" is local to one browser and is skipped.
  - Resolutions are skipped (`PV_UI_TEST_VIDEO_MODES=1` includes them).
- **`mask`:** draws a box on the stream, resizes it from the bottom-right and top-left corners
  (the opposite corner stays put), moves it, adds a second box, removes the first with Delete, then
  Remove all. Each step is checked against the backend's box coordinates (within 2%) and the
  second dashboard's box table. 10 s.
- **Proved against the bug:** a jar with `-39` undone (`updateStore` false again) fails exactly
  Contrast, Gamma and Sharpness with "page didn't show the new value", and Backlight Compensation
  with "didn't go back". The good jar then passes.
- **Deadlines:** 10 s for any click or fill (`actionTimeout`; Playwright's default is none, and a
  stuck locator hung the first run silently), 2 min a test, 8 min a run (`globalTimeout`), and
  `run.sh` kills anything past 10 min.
- **Left-behind check:** `global-setup` records every camera's running pipeline; `global-teardown`
  fails the run if one ends elsewhere or a `zz-uitest` pipeline remains. Added after a crashed
  cleanup left TopRight on its object-detection pipeline, which later runs then treated as normal.

#### Extra camera control sliders snapped back (`photonvision-39`)

Contrast, gamma, sharpness and backlight compensation (`photonvision-28`) show the store's value
one-way (`:model-value`, not `v-model`). But `setExtraControl` sent the change with
`updateStore = false`, so the camera changed (the preview did) while the slider and its number
went back to the old value. Now it updates the store.

For comparison, one camera alone took 1.45 ms and 7% GPU at the start.

#### First stage as one CUDA graph (`bos-05`)

- **What:** `GpuDetector::RecordFirstStageGraph` records the fixed-size first stage once per
  detector: the labels memset, threshold and decimate (4 kernels), labeling (5 kernels), the
  point-count memset, `BlobDiffCompact`, and the count's copy to the host. `Detect()` then launches
  it as one graph: 13 CUDA calls a frame become 1. `FirstStageGraphWanted` asks for a new
  recording if the input's GPU buffer or `min_white_black_diff` changes (at most 8 per detector
  slot, then it runs ungraphed). Used only with the fused `BlobDiff`, a gray image and no event
  timing; `0` in `/tmp/spectrum-971-graph` goes back to launching the steps one by one.
- **Recording must happen with no other CUDA work in the process.** The first version recorded
  inside `Detect()`, on the first frame. When all 4 cameras started at once, that broke CUDA
  calls on the other threads, which aren't allowed while any stream is capturing:
  - a scan on CUDA's legacy stream ("operation would make the legacy stream depend on a capturing
    blocking stream"),
  - `cudaFree` from a detector rebuild ("operation not permitted when stream is capturing"),
  - the hardware decoder's `cuGraphicsEGLRegisterImage` (CUresult 900), which switched the
    hardware JPEG decoder off for the whole run.

  That happened on 4 of 10 starts. The A/B tests had missed it, because they switched the graph on
  live, after startup. Now `processimage` records it holding `CudaCaptureLock` exclusively.
  Every CUDA path in `lib971apriltag.so` holds that lock shared: detect, gray and colour decode,
  create and destroy. So does the TensorRT library, through `spectrum_cuda_lock_shared` /
  `spectrum_cuda_unlock_shared`, found with `dlopen(RTLD_NOLOAD)`. The lock is a
  writer-preferring `pthread_rwlock` (`PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`), since with
  8 threads taking it shared, glibc's default would let a recording wait indefinitely. Each
  recording logs `971 detector hN: first-stage graph recorded`. `fieldcal_detect` (one thread)
  records right before its first `Detect()`.
- **The line-fit scan's stream (`bos-06`).** `cub::DeviceScan::InclusiveScanByKey` in `Detect()`
  had no stream argument, so it ran on the legacy default stream. That stream waits for every
  other blocking stream in the process, and holds them up, so each camera's line-fit scan waited on
  the other cameras' GPU work. Now it runs on the detector's own stream.
- **Checked:** the same replay as `bos-04`, 3,567 detections, byte-identical with the graph on
  and off (with `bos-06`), and against the original build.
- **`fieldcal_detect` hung** during that replay once the CPU was busy. Its decode threads waited
  only for a ring slot to read free, and a slot also reads free while another thread is still
  decoding the frame one ring earlier into it. Two threads then filled the same slot, one
  "ready" was lost, and the detect loop waited forever (it could also have detected the wrong
  frame's pixels). Now a thread starts frame `i` only after the detect loop has consumed frame
  `i - ring`.
- **Measured live** (alternating 3 times): GPU **25.6%** against 27.6% (the gaps between 13 small
  launches), detect 1.20 against 1.23 ms, CPU unchanged (1.23 cores). The remaining CPU isn't
  launch overhead any more (below).

#### Where the remaining CPU goes (4 cameras, 1.2 cores)

- **`tests/cpu-profile.sh`:** the 4 camera threads (15–25% of a core each) split their samples
  between `grabRawSinkFrameTimeoutLastTime` (waiting for the camera), `decodeMjpegGray` and
  `processimage`.
- **Page faults** ~1/s: buffers are reused, allocation costs nothing.
- **System calls** (function tracer, 0.36 s; the kernel refuses `set_ftrace_filter`, so it traced
  everything), per second: futex ~44,000, `mprotect` ~11,000, `sched_yield` ~9,800, `getpid`
  ~9,800, ioctl ~8,700, openat/close ~1,600/1,900.
- **Where from** (gdb `catch syscall`): `mprotect` and `sched_yield` come from inside libcuda's
  `cudaEventSynchronize`, which is how its blocking wait works. `getpid` and `openat` come from
  NVIDIA's JPEG engine driver (`libnvvideo` → `libnvrm_host1x`), which opens `/dev/dri` and
  `/dev/dri/renderD128` and pushes host1x command streams for every frame, on one helper thread
  per camera (~2.4% of a core each). Both are inside NVIDIA's libraries.

#### Latency: where a frame's ~14.5 ms go

The dashboard shows 14.3–15.2 ms per camera: mid-exposure to the result.
- **New in the `971 jpeg` line:** `frame age at decode` is how old each frame is when its decode
  starts, from cscore's timestamp. That's `WPI_TIMESRC_V4L_SOE` (source 3), which our driver sets
  at the frame's first USB packet. The line also has `JPEG avg` (KB).
- **Measured** (4 cameras, 122 fps): age at decode 8.2 ms on average (max ~16), JPEG 34–36 KB.
- **Split with the kernel log:** `/tmp/spectrum-971-kmsg` makes the decoder write each decode start
  to `/dev/kmsg`, next to uvcvideo's "Frame complete" lines (`trace=128`). Over 393 ms:
  **first packet to last packet 8.1 ms** on every camera, **last packet to decode start
  0.15 ms**.
- **So the camera sets the pace, not USB and not PhotonVision.** At alt 7 (1280 bytes per 125 µs)
  34 KB would cross in 3.4 ms, but the camera spreads each frame over ~8.1 ms, about one frame
  period: it sends the JPEG while the sensor reads out. So the bandwidth cap doesn't add the ~2 ms
  latency estimated in VISION-RESEARCH.md.
- **Budget:** ~2.5 ms (half the 5 ms exposure) + 8.1 ms (camera readout and send) + ~2.8 ms (JPEG
  decode, 4 cameras sharing the 2 engines) + 1.2 ms (detection) ≈ 14.6 ms.

#### A camera stuck at 320x240, again: the real cause (`photonvision-37`)

`photonvision-27`'s recovery never fixed it. Twice tonight (00:12 and 01:14) TopRight came up at
320x240; the "reconnecting it so the mode is applied again" warning repeated every 3 s for minutes,
and only a PhotonVision restart helped.
- **Chain of events,** from the logs and cscore's source
  (`allwpilib-v2026.2.1/cscore/src/main/native/linux/UsbCameraImpl.cpp`):
  1. On first connect, with no mode set yet, cscore applies the camera's **lowest** mode:
     "set format 1 res 320x240" (`DeviceCacheMode`).
  2. PhotonVision sets 1280x800. The resolution changed, so cscore closes the device, reopens it
     and calls `VIDIOC_S_FMT`, which failed with **`Device or resource busy`** both times, and so
     did `VIDIOC_S_PARM` (cscore logs these as `ioctl VIDIOC_S_FMT failed ...`). In uvcvideo,
     EBUSY there means another handle still owns the stream: the old one, not yet released.
     Probably a control read from another thread still held it (PhotonVision caches and sets
     properties at the same moment). No other process had the camera open, and Java's
     `ProcessBuilder` closes inherited file descriptors.
  3. cscore keeps 1280x800 as its mode anyway, so `USBFrameProvider` sees 320x240 frames against
     1280x800.
  4. `GenericUSBCameraSettables` logged "Failed to set video mode!" when `setVideoMode` returned
     **true** (success), so it printed on every start and hid the real failure.
  5. The recovery set `kForceClose`, then `kAutoManage`. In cscore's Linux camera loop that only
     stops and restarts streaming (`m_streaming && !IsEnabled()` → `DeviceStreamOff`). The device is
     never reopened and the format never sent again, and setting the same mode is a no-op.
- **Fix:** `reconnectForVideoMode` switches the camera to another mode of the same pixel format,
  then back (`camera.setVideoMode(other)`, `camera.setVideoMode(want)`). Two real changes, each
  reopening the device and setting its format, whatever state the race left. The warning now says
  "applying the mode again ... switching through WxH". The inverted log now warns only on a real
  failure.
- **Not yet seen in action:** 8 starts in a row after the fix came up clean (below).

#### A camera's thread killed at startup (`photonvision-38`)

- **What happened:** on one start, TopLeft's `VisionRunner` thread died with
  `ConcurrentModificationException` in `UIPhotonConfiguration.programStateToUi`. That's called
  from `VisionRunner.update` once its camera connects, and it iterates `VisionModuleManager`'s
  module list, a plain `ArrayList`, while `VisionSourceManager` is still adding cameras. The
  camera stayed at cscore's first-connect 320x240 and was never processed, until a restart. It's
  in the logs twice since 2026-09-24.
- **Fix:** the list is a `CopyOnWriteArrayList` (it changes only when cameras are added or
  removed), and `VisionRunner` catches any exception building the UI state, logging "Couldn't send
  the settings to the UI" instead of dying.

#### No copy of each gray frame (`photonvision-37`)

- **What:** `GrayscalePipe` copied every gray frame (1 MB at 1280x800) into `processedImage`,
  although it was already gray: 4 cameras × 122 fps ≈ 480 MB/s of memcpy. Now it shares the pixels
  (`Mat.assignTo`: OpenCV counts the references, so either Mat can be released first).
- **Where it matters:** only the dashboard stream draws on a frame (`OutputStreamPipeline` resizes
  and draws on both `colorImage` and `processedImage` in place), and so does Aruco's debug-threshold
  view. So `Frame.unshareProcessed()` gives `processedImage` its own copy right before a frame goes
  to the stream (at most 30 a second, `photonvision-15`) or to that debug view. Detection is always
  finished with the frame by then.

## Changes from the handoff

- **JetPack 6.2 → 6.2.3 (L4T 36.4.3 → 36.5.2).** Same Ubuntu 22.04 / CUDA 12 line,
  with bug fixes. JetPack 7.2.x now supports Orin, but it moves the Jetson to
  Ubuntu 24.04 / CUDA 13, which the fork and detector were not built for.
- The flash command adds `--erase-all`, per the 36.5.2 Quick Start.
- allwpilib is pinned to `v2026.2.1` instead of `main`.
- CUDA isn't part of a BSP-only flash; install `nvidia-jetpack` after first boot.
- The 2026 fork runs against 2027 alpha-6 robot code (see above).

## Open questions

1. Robot network: static IP for the Jetson on `10.85.15.x` (e.g. `.11`), or DHCP?
2. Answered: PhotonVision runs as a boot service (`photonvision.service`).
3. Answered: the fork is 2026-only, and it's used as-is (see above).

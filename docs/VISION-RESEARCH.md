# Vision research: other systems, and what we changed

What other FRC vision systems do that ours doesn't, what we measured on our own Jetson, what we
changed, and what's worth doing next. Researched and measured 2026-09-24. For the setup itself see
the [README](../README.md) and [TECHNICAL.md](TECHNICAL.md).

## Results first: where our CPU was going, and the fix

Profiling PhotonVision found that each camera's thread spent most of its time decoding the
camera's MJPEG frames, and that the decode was far more expensive than it needed to be:

- **cscore decoded every frame to full colour, then converted it to gray.** WPILib's cscore
  (2026.2.1, `Frame::ConvertImpl`) decodes any MJPEG frame to BGR first, even when the sink asks
  for grayscale. Its grayscale-only decoder (`ConvertMJPEGToGray`) exists but is never called.
  Measured on the Jetson from recorded frames: **8.9 ms per 1280x800 frame** that way, against
  **2.6 ms** decoding straight to gray with libjpeg-turbo. At 100+ fps that difference was most
  of a CPU core per camera, and it sat on every frame's latency path.
- **OpenCV's worker pool spun between jobs.** The colour-to-gray conversion ran on OpenCV's
  thread pool, whose workers busy-wait for their next job by default: 5 threads at ~22% each.
- **Our patch 02** ("gray capture") asked cscore for grayscale, which skipped a Java-side
  conversion but not cscore's colour decode, so the big cost stayed hidden until now.

Fixes:

1. `patches/photonvision-09-direct-gray-decode.patch` plus `decodeMjpegGray` in
   `detector/GpuDetectorJNI.cc`: PhotonVision takes the camera's own JPEG from cscore (as Rewind
   does) and our detector library decodes it straight to gray with libjpeg-turbo, into an image
   PhotonVision allocated.
   - A corrupt JPEG returns an error instead of crashing the JVM (libjpeg's default is `exit()`).
   - After 30 failures in a row, that camera falls back to cscore's conversion.
2. `09-robot-tuning.sh` step 9: `OPENCV_THREAD_POOL_ACTIVE_WAIT_WORKER=0` and `..._MAIN=0` for
   PhotonVision, so OpenCV's workers sleep instead of spinning. The pool is kept.

| 2 cameras, 1280x800, bench | TopLeft | TopRight | PhotonVision CPU (of 600%) | Latency (UI) |
|---|---|---|---|---|
| Before | 92 fps, 2.4 ms detect | 104 fps, 1.9 ms | 333% | ~23 ms |
| After | **122 fps**, 1.6 ms | **122 fps**, 1.6 ms | **129%** | **13 ms** |

Both cameras now run at their full 120 fps. CPU per frame went from about 17 ms to 5 ms, which is
what makes 4 cameras realistic. (Measured with `tests/perf-snapshot.sh`, which also reports
browser stream viewers, because a preview stream costs CPU.)

**Letting the CPU sleep while the GPU works, and more GPU work queues: no proven effect.** The
971 detector waits on the GPU ~8 times a frame, and CUDA's default wait busy-spins the CPU. With 2
cameras, switching it (`cudaDeviceScheduleBlockingSync`) saved no CPU and added ~0.3 ms of detect
time; `yield` was no better. With 4 cameras we also tried raising CUDA's hardware work queues from
8 to 32 (`CUDA_DEVICE_MAX_CONNECTIONS`), since the GPU was only ~31% busy yet the slowest frames
took twice the average. One run each made 32 queues look like a clear win (1–2 ms off the worst
frames). Repeating it showed the same settings vary by 2 ms between restarts, and over 11 runs 32
queues came out ~0.5 ms better and `block` no different. Both are now the defaults because neither
hurts. The lesson: one run per setting isn't enough when the noise is this big. Numbers in
[TECHNICAL.md](TECHNICAL.md) ("4 cameras: CUDA wait and GPU work queues"). Still switchable:
`SPECTRUM_971_CUDA_SYNC`, or `/tmp/spectrum-971-cuda-sync` + restart; `08-select-detector.sh
--gpu-connections N`.

### Exposure: 5 ms

The team found exposure **50 = 5 ms** works with the decision-margin cutoff lowered to **15**.
Shorter exposure means less motion blur, and tag36h11 with Hamming distance 0 almost never gives
false positives. The trade-off is flicker: under 120 Hz lights, an exposure that isn't a multiple
of 8.33 ms makes each frame catch a different part of the flicker cycle.
`tests/flicker-check/run.sh` measured **no flicker in the shop** at 5 ms: brightness changed 0.6%
frame to frame on average, 1% at most. **Rerun it under the event's lights.**

- New cameras now start with exposure 50 and decision margin 15 (`patches/photonvision-10-...`).
- The exposure slider now shows milliseconds for USB cameras. The raw number is in the UVC
  standard's 100 µs units, which is how 295 once looked reasonable but meant 29.5 ms and capped
  the frame rate.

## Other systems we looked at

### EagleEye (ElliotScully / Scythe-Engineering)

[Repo](https://github.com/Scythe-Engineering/EagleEye-Vision-System),
[Chief Delphi thread](https://www.chiefdelphi.com/t/eagleeye-photonvision-alternative-with-120-apriltag-fps-on-raspi-5/523976).
Python + Rust, Raspberry Pi 5. **PolyForm Noncommercial license: we can take ideas, not code.**
One author, alpha, not reported on a competition robot.

- **Main trick: region tracking.** It projects the field's tags through the last camera pose,
  warps each predicted region to a square crop, and runs the CPU detector only there, falling back
  to a full frame only when it finds zero tags. On *synthetic* frames it measured 5.9 ms against
  PhotonVision's 18–44 ms, excluding JPEG decode.
- **Why it doesn't help us:** our GPU detector does a full frame in ~1.6 ms, so there's almost
  nothing left to save. It also has recovery risks: a wrong pose after a collision searches the
  wrong places, and one lucky tag suppresses the full-frame fallback.
- **The one relevant idea was grayscale-only JPEG decode.** That turned out to be exactly our
  problem (above).

### Code Orange (3476): "MLTag"

[PR #2410](https://github.com/PhotonVision/photonvision/pull/2410), replaced by
[#2604](https://github.com/PhotonVision/photonvision/pull/2604), both open and targeted at
PhotonVision 2027.

- **It's not game-piece detection.** A YOLO model on an NPU (Rubik Pi / Orange Pi) finds the tags,
  and the CPU AprilTag detector reads only those crops, resizing big close tags down to ~200 px.
- **Their numbers:** about 2x on their 2026 robot, 50–60+ fps at 1280x800.
- **Same reason as EagleEye: little gain for us,** because we already detect on the GPU.

### Game-piece detection on the Jetson

**Built and working (2026-09-24):** see the README's game-piece section. Background research below.

- **[frc971/bos](https://github.com/frc971/bos)** has TensorRT YOLO (`src/yolo/yolo.cc`) and a
  **grayscale** model (`gray.engine`, 2025 game pieces). It projects detections to the floor and
  publishes `Pose2d`. It has no license file, so ask 971 before copying.
- **Grayscale matters:** our cameras are mono, so models must be trained on gray images of our
  own camera's frames.
- **No DLA on the Orin Nano,** so YOLO shares the GPU with AprilTags. Ultralytics' numbers for
  YOLO26n at 640 on an Orin Nano Super are 3.8–4.6 ms (INT8/FP16), plus pre/post-processing.
  One camera at 30 fps is realistic; all four at 120 fps is not.
- **Most practical path:** a separate TensorRT process on its own camera, publishing to
  NetworkTables. A TensorRT backend inside PhotonVision is the nicer end state and a lot more
  work.

### PhotonVision forks and upstream

- **Wire compatibility is better than we thought.**
  - The serde hashes changed with upstream [#2566](https://github.com/PhotonVision/photonvision/pull/2566)
    (WPILib alpha-7: timestamps became nanoseconds).
  - The last compatible upstream main commit is **a9ad078b**, already on WPILib 2027 alpha-6.
  - **Upstream v2026.3.4** is also still compatible (hash 4b2ff16a), and it merged with no
    conflicts onto the 4143 fork in a trial (our own patches not included in that test).
- **4533 "Whacknet"** ([fork](https://github.com/4533-phoenix/photonvision),
  [thread](https://www.chiefdelphi.com/t/4533-phoenix-whacknet-off-rio-constrained-solve-for-apriltags-zero-allocation-udp-vision/518777))
  solves the heading-constrained pose on the coprocessor, with the robot streaming its gyro over
  UDP at 200 Hz.
  - Porting it means leaving out its `messages.yaml` change, which would break the wire format.
  - Its `speedup` branch only reconfigures the detector when settings change; that part is cheap
    to take.
- **frc971 cos** decodes MJPEG with the Jetson's hardware JPEG decoder, through NVIDIA's
  `NvJPEGDecoder` class, which calls `exit()` on a bad JPEG. We built our own (see Jetson-specific
  findings).
- **STEM-Alliance and joelamaldas forks** duplicate what we already have, including recording.
- **PhotonLib alpha-2 already has `PhotonCamera.setEnabled()`**
  ([#2484](https://github.com/PhotonVision/photonvision/pull/2484)), but our 2026-based
  PhotonVision ignores it. Porting the server side (#2484/#2499) is small and safe.
- **Other upstream fixes worth taking:**
  - OpenCV native-leak sweep [#2511](https://github.com/PhotonVision/photonvision/pull/2511)
    (conflicts; hand-port)
  - CVMat refcount fix [#2356](https://github.com/PhotonVision/photonvision/pull/2356)
  - NT reconnect-hack removal [#2398](https://github.com/PhotonVision/photonvision/pull/2398)
  - calibration minimums [#2437](https://github.com/PhotonVision/photonvision/pull/2437) /
    [#2438](https://github.com/PhotonVision/photonvision/pull/2438)
  - ThriftiestOV9281 support [#2478](https://github.com/PhotonVision/photonvision/pull/2478)
- **Robot code:** avoid PhotonLib's heading-free constrained solve with an empty heading buffer
  ([#2529](https://github.com/PhotonVision/photonvision/pull/2529); alpha-2 throws).

### Jetson-specific findings

- **Measured (2026-09-24): each Thriftiest Cam reserves ~196 Mbps, in every mode.**
  - It always picks UVC alternate setting 11 (3 × 1,020 bytes per 125 µs microframe = 24.5 MB/s),
    even at 640x400, and its MJPEG modes only offer 120 fps.
  - It actually uses 4–7 MB/s.
  - USB 2.0 caps isochronous reservations at about 80% of a root port, so **each root port holds 2
    of these cameras.**
  - The USB-C port with a hub is a separate root port (xHCI Bus 01 root port 1; the USB-A ports'
    built-in hub is root port 2). A camera there streamed 121 fps at 1280x800 while TopLeft kept
    detecting at 122 fps.
  - So: **2 AprilTag cameras on USB-A + 2 on USB-C = 4, and that's the USB 2.0 budget.** A 5th
    camera (game pieces) needs to be USB 3, which uses the separate SuperSpeed bus (Bus 02), or has
    to reserve very little bandwidth (test it next to two cameras).
  - USB-C is also the laptop cable, but SSH over Wi-Fi works; only flashing and backups need USB-C.
- **4 cameras on the USB-A ports would fail with the stock driver** (the numbers above).
- **Fixed with a capped driver** (`kernel/uvcvideo-payload-cap.patch`,
  `scripts/jetson/11-uvcvideo-payload-cap.sh`).
  - Linux's `uvcvideo` reserves whatever the camera requests (`dwMaxPayloadTransferSize`). It only
    recomputes that for uncompressed formats (`UVC_QUIRK_FIX_BANDWIDTH`), never for MJPEG.
  - The patch adds a `payload_cap=vid:pid:bytes` module parameter. We use `1bcf:28c5:1280`, which
    picks alternate setting 7: 2 x 640 bytes per microframe = 10.24 MB/s = ~85 KB per frame at
    120 fps. Four cameras reserve 5,120 bytes per microframe, within USB 2.0's ~6,000.
  - Built from stock v5.15.199 source; the unmodified build's `srcversion` matches NVIDIA's
    installed driver exactly (51AFB22511907605800B082). It's installed in
    `/lib/modules/<kernel>/updates/` with the option in `/etc/modprobe.d/`. `--undo` restores stock.
  - **Tested with 2 cameras:** both on alt 7, 122 fps each. A 10 s recording had every frame
    complete, no gaps over 45 ms, and no invalid-JPEG warnings. Frames were 48–50 KB median,
    **62 KB max** (73% of what alt 7 carries at 120 fps). A busier scene makes bigger frames; if
    frames approach 85 KB, use `CAP=1bcf:28c5:1984` (alt 9, 3 per port) or put cameras on USB-C.
  - **Trade-off (estimated):** a frame would take longer to cross USB, ~4.9 ms for 50 KB at alt
    7 against ~2.0 ms at alt 11. **Measured 2026-09-29, it doesn't:** the camera spreads each
    frame over ~8.1 ms (about one frame period, while its sensor reads out), longer than alt 7
    needs for a 34 KB frame (3.4 ms), so the cap adds no latency (TECHNICAL.md, "Latency").
    Timestamps aren't affected: the driver
    stamps a frame when its *first* USB packet arrives, and `photonvision-13` moves that back to
    mid-exposure.
- **Hardware JPEG decode (NVJPG) works on our Orin Nano, but only one way of calling it gives
  the right frames.** Tested 2026-09-24 on L4T R36.5.2 with 156 recorded 1280x800 frames, each
  output checked pixel for pixel against libjpeg-turbo.
  - **There are two engines, not one.** Two decoders side by side don't slow each other, and 4 at
    once reach ~950 fps in total (4 x 120 fps needs 480). NVIDIA's datasheet doesn't list NVJPG
    at all; its clock tables do (499.2 MHz). There's no hardware encoder.
  - **AOS's method returns frozen frames here.** AOS reports **2.3 ms a frame with 0.22 ms of
    CPU** on JetPack 6.2 ([commit ec9719d002](https://github.com/RealtimeRoboticsGroup/aos/commit/ec9719d002)).
    Their method (libnvjpeg writes the rows into our buffer, one decoder reused) gave us frame 0
    for all 156 frames, while libnvjpeg still reported a hardware decode. It may depend on the
    L4T version. A new decoder per frame gives correct frames but takes 22 ms.
  - **What works:** decode into libnvjpeg's own buffer (2.3 ms), then CUDA copies the gray plane
    out, attaching the buffer again every frame: **2.9 ms and 0.95 ms of CPU a frame**, against
    2.9 ms and 2.9 ms for libjpeg-turbo. Attached only once, 117 of 156 frames were stale. A CPU
    copy is also correct but slow, because the buffer is uncached: 3.6 ms and 1.7 ms of CPU.
  - **Pitfalls:** NVIDIA's `NvJPEGDecoder` class calls `exit()` on a bad JPEG, which would kill
    the JVM. After a decode error the decoder must be re-created, or every later frame fails.
    libnvjpeg exports the same function names as libjpeg-turbo, so our detector library can't
    link both. The forums also report clock and chroma-format bugs.
  - **The detector can read the decoder's buffer directly.** bos's `Detect(host, device)` takes a
    GPU pointer; our JNI passes `nullptr` today. On 2,856 recorded frames (358 with tags) the
    detections were identical to today's. The whole chain takes ~4.8 ms and 3.0 ms of CPU a
    frame, against ~5.0 ms and 5.1 ms with libjpeg-turbo, and there's no copy to the GPU. But
    against the hardware decode we now run, it gains only ~0.15 ms and ~0.3 ms of CPU a frame:
    reading the decoder's buffer is slower for both the GPU and the CPU (TECHNICAL.md). Dropped.
  - **PhotonVision still wants a full-size image for the stream.** It shrinks every frame to
    213x133 for the dashboard, with no fps cap, even when no one is watching (~0.3 ms a frame).
    The direct path would give the stream its own small image instead.
  - **Built (2026-09-24), see [TECHNICAL.md](TECHNICAL.md).** With 2 cameras, PhotonVision fell
    from 0.85 to 0.52 cores. libjpeg-turbo decodes any frame the hardware can't, and one frame
    per camera every ~2 s is checked pixel for pixel against it.
  - **libnvjpeg must be in MJPEG mode** (`cinfo.mjpeg_decode = TRUE`, as NVIDIA's NvJPEGDecoder
    sets it). Otherwise it leaks ~250 KB a frame: PhotonVision was OOM-killed twice before we
    found it. MJPEG mode costs ~180 MB per decoder once, then stays flat.
  - **Colour cameras too:** NVJPG plus a CUDA kernel that repeats libjpeg's own upsampling and
    colour arithmetic, so the BGR output is identical to cscore's. One colour camera at 120 fps:
    1.12 → 0.59 cores. Only 4:2:2 JPEGs use it; tested on real colour photos, since our cameras
    are mono.
- **Tag range: the GPU detector looks for tags on a half-size image.** It finds tag outlines at
  640x400, then refines the corners and reads the ID at full size, so found tags keep full
  accuracy. The half size is hard-wired (`CHECK_EQ(quad_decimate, 2)`). Tested 2026-09-24: 40
  recorded frames of tag 3, shrunk to 40–8 px and re-compressed with the camera's JPEG tables,
  decision margin ≥ 15.
  - **Today:** 85% of frames at 20 px (~6 m face-on), 52% at 18 px, 2% at 14 px.
  - **Searching at full size:** 100% down to 14 px, 98% at 12 px (~10 m), 70% at 10 px. That's
    about 1.7x the range. Shrunk tags are sharper than real far-away ones, so real distances are
    shorter for both. Tested by doubling the image (nearest-neighbour), so the detector's halving
    gives back the original. The CPU AprilTag library agreed.
  - **Cost** (cameras as threads in one process at 120 fps, on top of PhotonVision's own two):
    2 cameras take 58% GPU and 4.4 ms a detection, against 38% and 1.6 ms today. 4 cameras
    couldn't keep up (72 fps). That's an upper bound: a real full-size mode needs detector
    changes, and would be cheaper.
  - **Not now.** It fits 2 cameras, not 4, and less with YOLO. First check what distance the robot
    code trusts: a 12 px tag gives a noisy single-tag pose. Cheaper options: full size only on the
    cameras facing downfield, or only in a band around the horizon, where far tags appear.
- **CSI cameras:** the devkit has 2 connectors; Arducam's OV9281 does 80 fps at 1280x800 (slower
  than our USB 120 fps). No JPEG step, and hardware timestamps. Not planned (team decision,
  2026-09-25): only if USB stops being enough.
  - The 4143 fork's camera table already lists Innomaker OV9281 MIPI cameras on the Orin (the
    `vi-output, ov9281` V4L2 devices), and it re-enabled gray video modes for them.
  - So PhotonVision may run them through plain V4L2. Untested by us.
- **Lower-jitter options (971's approach):** pin USB interrupts to one core, run the capture
  threads at real-time priority, and disable the deepest CPU idle state. This helps worst-case
  latency, not the average. Low effort.
- **Doesn't apply to us:** VPI/PVA AprilTags (the Orin Nano has no PVA), region tracking,
  uncompressed USB 2 video, and Java GC tuning (our heap sees no collections).

## Second pass: AOS and the teams running it, bos and cos, Northstar

Researched 2026-09-24 by four Claude subagents, reading the code, commits and Chief Delphi posts.
Ratings are **October event / next season**.

### Austin Schuh's AOS (1868, 4646, 254, 2910)

[RealtimeRoboticsGroup/aos](https://github.com/RealtimeRoboticsGroup/aos) is Austin's current
vision stack: `frc/orin/` (the CUDA detector) and `frc/vision/` (logging, replay, calibration,
localizer).

- **Who runs it:**
  - **1868:** runs upstream `main` directly; their students' commits land there.
    [team1868/aos](https://github.com/team1868/aos) is just upstream as of 2026-01-17.
  - **4646:** their swerve localizer is now upstream.
  - **254:** ran a fork in 2026 ([Team254/aos-public](https://github.com/Team254/aos-public),
    a January 2026 snapshot; their in-season code isn't public). They used Limelight 4 in 2025.
  - **2910:** starting now. Their [orin-os](https://github.com/FRCTeam2910/orin-os) fork of the
    1868 image (2026-09-22) only adds mDNS and DHCP. Their 2026 robot used Limelight 4, and their
    Orin vision is "not far enough along to share".
- **The image:**
  - A Yocto build ([meta-frc4646 `frc1868-walnascar`](https://github.com/frc4646/meta-frc4646/tree/frc1868-walnascar),
    JetPack 6.2) with an XFS root, chrony, and udev names by USB port.
  - Bazel cross-compiles the vision code and deploys it (`bazel run //frc/vision:download_stripped`).
  - "254's Bazel images" are this setup. The OS image is Yocto; Bazel builds the programs.
  - No A/B updates and no read-only root.
  - 1868 is porting it to JetPack 7.2 ([in progress](https://github.com/anikadata/meta-frc4646/tree/anikadata/frc1868-wrynose)).
- **Same hardware and choices as ours:**
  - Orin Nano, 4 UVC MJPEG cameras at 1280x800 and 120 fps, 5 ms exposure (1868's public config).
  - CPU gray JPEG decode, the 971 detector, 8 distortion coefficients.
  - A uvcvideo bandwidth patch: theirs divides the reservation by 8 (16 on 1868's new branch);
    ours caps it at alt 7.
- **The detector:** neither 1868 nor 2910 changed it. Our bos copy matches upstream, including
  the 2026-03-30 host-copy fix.
  - Austin's "massive speedup" tuning is `min_white_black_diff` 20 plus min cluster pixels ≥ 24.
  - We already run it: 20 is set in the service, and the GPU code enforces at least 24.

**What AOS has that we don't:**

| Feature | Where | Oct / next | Why |
|---|---|---|---|
| **Logging that starts itself.** Every camera's raw MJPEG from enable to 10 s after disable, named by event and match from FMS, in 5 s chunks, stopping below 50 GB free. 254 does the same from the `/AdvantageKit/DriverStation` topics our robot code already publishes. | `frc/vision/image_logger.cc` | **High / high** | Rewind already records. Starting on enable and naming by match is a small change, and no match gets missed. |
| **Replay:** logged frames run back through the real detector, with MCAP output for Foxglove | `image_replay.cc`, bos `src/camera/disk_camera.cc` | Med / **high** | Tune thresholds and regression-test patches on real match footage. |
| **Edge rejection:** drop a tag if any corner is within 25 px of the image edge (`--pixel_border`; the code default is 50) | `frc/orin/gpu_apriltag.cc` | Med / med, **as a robot-side trust falloff** | Distortion is strongest and calibration weakest near the edge, and tags there are often cut off. A hard drop throws away ~10% of the image, so we'll trust edge tags less on the robot instead, with tunable dials (issue #10, 8b). No Jetson change. |
| Minimum decision margin **50** (ours is 15, team-tuned at 5 ms) | same | Med / med | A data point for field tuning. Check false positives in Rewind footage before choosing. |
| Frames older than 55 ms dropped instead of queued | same | Low / med | Keeps latency bounded if the Jetson is overloaded. |
| **Noise per detection:** how far undistortion moved the corners, pose-error ratio, noise growing with distance, per-tag trust, counters for 10 rejection reasons | `swerve_localizer/localizer.cc`, `status.fbs` | Med / high | Robot-side, from PhotonLib's corners (issue #10). |
| **Health telemetry:** temperatures, fan, power rails every 5 s; good/failed JPEG decodes per camera; free disk | `frc/orin/hardware_monitor.cc`, `turbojpeg_decoder_status.fbs` | **Done** (Jetson side) | On NetworkTables: temperatures, fan, power, throttling and over-current (`photonvision-17`), each camera's fps, decode failures, recoveries and problem (`-29`, `-31`), and Rewind's free disk. The robot code still has to log them and raise alerts. |
| **Camera-mount calibration from data:** spin the robot between two ChArUco diamond targets | `calibrate_multi_cameras_lib.cc`, [971's procedure](https://github.com/frc971/971-Robot-Code/blob/master/y2024/vision/README.md) | Med / high | Measured robot-to-camera transforms beat CAD numbers. |
| **Field tag map from logs** (Ceres solve) | `target_mapper.cc` | Low / med | WPILib's [WPIcal](https://docs.wpilib.org/en/stable/docs/software/wpilib-tools/wpical/index.html) measures tag positions from video, so Rewind footage could feed it. |
| Hardware JPEG decode | see above | **Done** | On since 2026-09-24 ([TECHNICAL.md](TECHNICAL.md)). Only one way of calling it gives the right frames. |
| Live focus score while turning the lens | bos `src/calibration/focus_calibrate.cc` | Med / low | Cheap, and a sharper image helps every tag. |
| USB interrupt pinning, real-time priorities | | Low / low | Less jitter, but risky a month out. |
| Exposure set by alliance side | `field_side_exposure_adjuster.cc` | Low / low | It's effectively off in their config. |
| Reproducible Yocto image and pinned Bazel sysroot | meta-frc4646 | Low / low-med | Our backup image and GitHub release already cover recovery. |

**Where we're ahead:**
- **Timestamps:** ours mark mid-exposure. Theirs are raw V4L2 times with no exposure correction,
  and 254 stamps at the end of the frame.
- **Time sync:** ours is a round trip. 254's is one-way.
- **Reliability:** a hardware watchdog and power-cut-tested settings. They have neither
  (unverified for 1868).
- **Game pieces:** ours run on the robot's cameras today. 254's YOLO is a stub, and 1868's isn't
  public.
- **Robot code:** stock PhotonLib, with no custom UDP protocol.

### 971's bos and cos

- **bos** [`unambiguous_estimator.cc`](https://github.com/frc971/bos/blob/main/src/localization/unambiguous_estimator.cc)
  resolves single-tag ambiguity across cameras.
  - It tries every combination of each camera's two candidate poses and keeps the set that best
    agrees with itself and with the last pose.
  - `joint_solver.cc` (all cameras in one solve) is unfinished. **Low / med.**
- **bos's game-piece loop** opens its own camera and is only used in a test.
- **cos** (971's rewrite, July 2026) decodes each camera once in hardware.
  - The frame goes to AprilTag detection and, separately, to a YOLO loop, so YOLO's timing never
    delays AprilTags ([design](https://github.com/frc971/cos/blob/main/full_gamepiece_design.md)).
  - It doesn't publish results yet.

### 6328 Northstar

The newest code is [RobotCode2026Public/northstar](https://github.com/Mechanical-Advantage/RobotCode2026Public/tree/main/northstar)
(2026-08-19). It now runs on a Mac mini.

**Coprocessor features:**
- **Robot-owned config and tag layout.** Robot code publishes each camera's settings and the tag
  layout JSON over NetworkTables. A dashboard chooser switches to subsets (hub-only, none), and
  the coprocessor re-solves. **Med / high:** our multi-tag solve uses the Jetson's layout, so at
  an event the robot can't drop one badly placed tag.
- **Self-healing cameras.** Before every restart it power-cycles and re-enumerates the USB port
  (`uhubctl`), and it exits after 3 s of failed grabs. **Done** (`photonvision-29`): a camera
  with no usable frames for 3 s is reconnected, then reset at the USB level 5 s later, like a
  replug.
- **Throttle to 1 fps after 5 s disabled.** **Low / med:** less heat in the queue.
- **Annotated match video**, with tags and detections drawn on, named by match. **Low / low:**
  we can draw overlays at export time from logged corners.
- **Tiled object detection:** three overlapping 640 tiles plus a downscaled full frame. **Low / med:**
  finds small, far game pieces at several times the inference cost.
- **Uncompressed USB 3 cameras** (Basler): about 30% less corner noise than MJPEG
  ([CD](https://www.chiefdelphi.com/t/frc-6328-mechanical-advantage-2026-build-thread/509595/943)).
  **Low / med:** a hardware choice, to weigh against our 120 fps.
- **Calibration and event routine:**
  - Lock focus with silicone before calibrating.
  - 50+ images weighted to the edges.
  - Check the undistorted image by eye.
  - At events, check tag IDs, use minimum exposure with gain, and verify the pose at the scoring
    spots.
  - **High / high:** it costs nothing.

**Robot-code practices** (for the robot-code agent, issue #10): std devs of 0.01·d²/n² (xy) and
0.03·d²/n² (θ), with single-tag θ ignored. The rest:
- **Single-tag disambiguation:** keep a frame only when one reprojection error is under 0.4x the
  other, then pick the candidate closest to the gyro.
- **Sanity gates:** drop poses more than 0.5 m outside the field, or with z outside −0.5 to 1 m.
- **Early auto:** ignore vision for the first 2 s.
- **Alignment:** a separate single-tag gyro + tx/ty estimate for final alignment.
- **Moving cameras:** camera transforms looked up by timestamp for cameras on mechanisms.
- **Health alerts:** "not on NetworkTables" reported separately from "no frames for 1.5 s".
- **Game pieces:** a field map where pieces expire after 3 s.
- **Bumps:** odometry trust scaled down with tilt.

### 3015's Polaris (checked 2026-09-28, nothing to take)

[3015RangerRobotics/polaris](https://github.com/3015rangerrobotics/polaris) is their fork of
Northstar's 2024-era design. **Low / low.** Don't search it again.
- **Stack:** Python on Orange Pis, about 45 KB of code, MIT. OpenCV ArUco detection on the CPU,
  `solvePnPGeneric` (IPPE_SQUARE) for two candidate poses, robotpy to NetworkTables, and an MJPEG
  stream.
- **Setup:** `setup.sh` builds OpenCV with GStreamer on each device and installs a systemd
  service. The image workflow is unfinished ("maybe builds an image").
- **Missing:** game pieces, health telemetry, camera recovery, logging and replay.
- **Activity:** last push 2024-04-03. It has 9 saved camera calibrations, so they ran many cheap
  cameras.
- **Vs. ours:** a weaker detector than the 971 one on a much slower platform. No performance
  numbers or field results are published.

## If we designed the ideal FRC vision system

What an ideal system has that ours doesn't yet, roughly in order of value for effort:

1. **Every match recorded without anyone remembering to.** Rewind starts on enable, stops a few
   seconds after disable, and names files by event and match.
2. **Replay.** Run any recording back through the exact detector and settings to tune thresholds
   offline and to test every patch against real match footage before it ships. **Partly done:**
   `fieldcal_detect` replays Rewind recordings through the 971 detector at ~400 fps, with its
   thresholds as flags.
3. **Cameras that heal themselves.** Detect a stuck or corrupt camera and reset its USB port
   automatically, instead of warning. **Done** (`photonvision-29`).
4. **Quality metadata on every tag,** so the robot can weigh each one: distance from the image
   edge, how far undistortion moved the corners, reprojection error, decision margin.
   - Tags near the edge are trusted less, on a tunable curve (robot side).
   - The robot chooses which tags to trust.
5. **Health in the robot log:** temperatures, fps, failed decodes, USB resets and free disk, as
   dashboard alerts. The Jetson publishes all of it to NetworkTables now. What's left is the robot
   code logging it and raising alerts.
6. **Calibrated from data, not CAD.**
   - Camera mounts measured by spinning the robot in front of targets.
   - The event's real tag positions measured from Rewind video (WPIcal).
   - Focus checked with a live sharpness score.
7. **AprilTags and game pieces from the same cameras,** in field coordinates and tracked over time
   (see Future work).
8. **Latency measured, not estimated:** the robot spin test, or an LED blink seen by the camera.
9. **Cameras exposing at the same instant** (hardware trigger), so observations from different
   cameras share one timestamp. Needs cameras with a trigger input.
10. **A joint multi-camera solve** that uses the gyro: all cameras' corners in one fit.
11. **Hardware JPEG decode,** to free CPU for more cameras. Done for the gray cameras; the colour
    camera is next.
12. **A reproducible image built from source** (Yocto or similar), instead of a configured stock
    image. Our backups make this the least urgent.

## Future work

**AprilTags and game pieces in one pipeline (team note, 2026-09-24).** It feels like it should be
easy, and it's worth benchmarking.

- **Why it's possible:**
  - The Wave FUEL model found balls well on our mono camera.
  - PhotonVision's result already carries both kinds of target.
  - AOS already does it: each camera is decoded once, keeping every 3rd frame, into shared
    memory. The AprilTag and YOLO processes both read that gray image without copying it, and
    YOLO works on a 512x416 crop.
  - cos is designed the same way. No team has published timings for both together.
- **What needs building:**
  - The AprilTag pipeline decodes straight to gray, and our TensorRT runner requires 3 channels.
    Convert the 640x640 letterboxed image `GRAY2BGR` first: well under 1 ms, and it's what the
    model saw in our test.
  - Run AprilTags on every frame. Hand every Nth frame (4th = 30 fps) to an async YOLO worker on
    its own CUDA stream, so inference never delays an AprilTag result.
  - Publish game pieces with the timestamp of the frame they came from. Either use a separate
    result (a "virtual camera" such as `TopLeft-pieces`), or attach them to a later frame's
    result with their own timestamp.
- **Benchmark:**
  - AprilTag fps, detect time (average and worst) and latency on every frame.
  - YOLO fps and latency.
  - GPU and CPU use.
  - Setups: 1, 2 and 4 cameras doing both, against AprilTags only.
  - No engine builds running (see the README).

**Field calibration mode (solver built 2026-09-24):** during field calibration, push the robot by hand to
10–20 spots. One solve then gives the event field's real tag positions, every camera's
`robotToCamera`, and settings recommendations. Plan: [FIELD-CALIBRATION-PLAN.md](FIELD-CALIBRATION-PLAN.md).
The solver is [tools/fieldcal](../tools/fieldcal/README.md). It runs on the Jetson, replaying
recordings through the 971 detector, or on a laptop.

**Before October (cheap):**
1. Test 3–4 cameras when they arrive (`tests/perf-snapshot.sh`).
2. Rewind auto-start on enable, named by event and match (robot half: issue #10).
3. Trust tags less as they near the image edge, on the robot (issue #10, 8b), instead of dropping them on the Jetson.
4. ~~Reset a stuck camera's USB port automatically.~~ Done (`photonvision-29`).
5. ~~Per-camera failed-decode counts and temperatures in NetworkTables.~~ Done; the robot code
   still needs alerts on them.
6. The calibration and event routine above. Rerun `tests/flicker-check` under the event lights.
   Compare decision margin 15 with 50 on Rewind footage.
7. Robot-side filters and std devs (Northstar and AOS lists above) → issue #10.

**Next season:**
- A replay tool for Rewind recordings.
- Camera-mount calibration and field mapping from data.
- Hardware JPEG decode: check it with a real colour camera's recordings, and with 4 cameras.
- Full-size tag search for range, if the robot code will use far tags (see Jetson-specific
  findings).
- A joint or heading-constrained solve (Whacknet, bos).
- IRQ affinity and real-time priorities.
- USB 3 or triggered cameras (CSI isn't planned).
- JetPack 7 (1868 is porting their image now).
- Basing on upstream PhotonVision 2027 main with our CUDA pipeline, instead of the 2026-based 4143
  fork.

**Done since the first pass:**
- `setEnabled()` (patch 11)
- upstream v2026.3.4 (patch 00)
- OpenCV leak fixes (patch 12)
- game pieces with TensorRT (patch 14)
- the USB bandwidth cap
- mid-exposure timestamps (patch 13)
- hardware JPEG decode (NVJPG), with libjpeg-turbo as the fallback

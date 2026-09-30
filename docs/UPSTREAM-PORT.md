# Upstream PhotonVision port (patches 00, 11, 12)

What we brought in from upstream PhotonVision on 2026-09-24, and what to watch for. The port was
done by a Claude subagent in a separate clone (`~/build/pv-upstream`), then built with our real
build script and bench-tested on the Jetson. For the setup itself, see the [README](../README.md).

## The short version

- **`photonvision-00-upstream-v2026.3.4.patch`:** the full diff from the 4143 fork (`d8c9e8e`) to
  a merge of upstream **v2026.3.4**, 41 upstream commits. It still speaks the exact same
  NetworkTables format: all six serde hashes are unchanged (table below), so PhotonLib
  v2027.0.0-alpha-2 on the robot still works.
- **Our 01–10** are regenerated on top of it. Only 05 and 07 changed in content: upstream #2407
  changed how the UI checks for dark mode.
- **`photonvision-11-set-enabled.patch`:** the server side of PhotonLib's
  `PhotonCamera.setEnabled()` (#2484, #2499). It did nothing on our build before.
- **`photonvision-12-opencv-leaks.patch`:** the OpenCV native-memory leak fixes from #2511 that
  matter on our every-frame paths, plus two leaks upstream still has.

### Bench check (2026-09-24)

- All 13 patches applied on the build script's own clone, and the jar built.
- On the Jetson: both cameras at ~121 fps with calibrations loaded, and no "CVMat was GC'd
  without release" warnings.
- Rewind records, and the Settings DB has no pre-2024.3 quirk names (#2412).
- The static gateway is now `10.85.15.4` (#2364, below).

## Serde hashes (the hard constraint)

| Message | before (d8c9e8e) | after (00–12) |
|---|---|---|
| PhotonPipelineResult | 4b2ff16a964b5e2bf04be0c1454d91c4 | same |
| PhotonPipelineMetadata | ac0a45f686457856fb30af77699ea356 | same |
| PhotonTrackedTarget | cc6dbb5c5c1e0fa808108019b20863f1 | same |
| MultiTargetPNPResult | 541096947e9f3ca2d3f425ff7b04aa7b | same |
| PnpResult | ae4d655c0a3104d88df4f5db144c1e86 | same |
| TargetCorner | 16f6ac0dedc8eaccb951f4895d9e18b6 | same |

`photon-serde/` and `photon-targeting/src/generated/` are byte-identical to d8c9e8e.

## Patch 00: upstream v2026.3.4

Highlights among the 41 commits:
- #2356: CVMat refcounting and a leak fix.
- #2364: static-IP gateway `.1` → `.4`.
- #2368: native library hash verification; extracted libraries are re-extracted if corrupt,
  which is good for power cuts.
- #2398: removes the NT reconnect loop.
- #2412: removes the old camera quirk aliases.
- #2429: sets raw exposure before auto exposure.
- #2338: configurable maximum target count.
- #2411: OV2311 auto-exposure quirk.
- Lots of UI, docs and build work.

The merge needed two fixes to build:
- **#2338 renamed `outputShowMultipleTargets` to `outputMaximumTargets`.** 4143's CUDA pipeline and
  `PipelineTypes.ts` still used the old name. Both are fixed, and the CUDA pipeline gets the same
  127-target cap upstream gave the AprilTag pipeline.
- **v2026.3.4 pins dev snapshots of the rknn, rubik and mrcal libraries,** all since deleted from
  maven.photonvision.org. They're pinned back to the releases we already ship; mrcal, the
  calibration backend, is the same binary as before.

## Patch 11: `setEnabled`

- **Topics,** per camera, under `/photonvision/<camera nickname>/`:
  - `enabledRequest` (boolean): the robot writes it with `setEnabled()`. It defaults to `true`.
  - `enabled` (boolean): PhotonVision publishes it with every result.
- **When disabled,** the pipeline is skipped and an empty result goes out. The heartbeat stays at
  0, so **PhotonLib's `isConnected()` goes false after 0.5 s.** That's upstream's behavior: robot
  code that treats `isConnected()` as health will see disabled cameras as disconnected.
- **Where we differ from upstream,** each marked `SpectrumJetson` in the code:
  - A disabled camera also skips the frame grab, which saves our ~3 ms decode.
  - The FPS-limit sleep stays after processing, since upstream's move added up to a frame of
    latency.
  - The thread's interrupt flag is kept.
  - Leaked listeners are removed on camera rename.
- **Rewind keeps recording while a camera is disabled** (it has its own sink). The CUDA detector
  stays allocated, so re-enabling is immediate.
- **Not ported:** the web-UI warning for disabled cameras.

## Patch 12: OpenCV leaks

From #2511, only the fixes on paths that run every frame:
- `TrackedTarget` temporaries, and releasing the targets' native Mats after the output stream has
  drawn them (they were never released).
- `Draw2dTargetsPipe` and `Draw3dTargetsPipe`.
- `TargetCalculations.calculateYawPitch`, and `OpenCVHelp` / `VisionEstimation`.

Plus two that upstream still has:
- the multi-tag first-pass targets in both AprilTag pipelines;
- the frame thrown away on a pipeline switch.

Skipped: the lifecycle refactor, the calibration rework (tangled with 2027 changes) and pipelines
we don't use.

## Patch 21: Thriftiest Cam support (#2478)

Upstream [#2478](https://github.com/PhotonVision/photonvision/pull/2478) (merged 2026-09-13, after
v2026.3.4) teaches PhotonVision about our camera (USB `1bcf:28c5`):
- **Three new quirks:** `ThriftyOV9281Controls`, `Gain`, `MJPEGOnly`.
- **`ThriftyOV9281CameraSettables`:** exposure range 1–2400 (0.1–240 ms) and continuous autofocus
  off. The lens is fixed-focus anyway.
- **Ported to our 2026 base:** `edu.wpi.first.cscore` names instead of 2027's `org.wpilib`
  (`PixelFormat.kMJPEG`).
- **Upstream's test:** `QuirkyCameraTest.thriftyOv9281Test` passes (3/3).

**Our addition: saved cameras get the new quirks too.**
- PhotonVision detects quirks only when a camera is first seen, so TopLeft and TopRight, saved with
  no quirks, would never get them.
- `USBCameraSource.addNewlyKnownQuirks` adds whatever the table now lists for a saved camera,
  except `MJPEGOnly`.
- **Why not `MJPEGOnly`:** it removes the YUYV modes, which would renumber the video-mode list.
  Every saved pipeline stores its resolution as an index into that list (ours: 13, 1280x800 MJPEG
  120 fps).
- **New cameras** (the ones on order) get all three quirks, with fresh indices.
- Existing quirks are never removed.

**Deployed 2026-09-24:**
- Both cameras logged "Added camera quirks [Gain, ThriftyOV9281Controls]" and use the Thrifty
  settables.
- `focus_automatic_continuous` went from 1 to 0.
- Exposure 50, brightness 64 and the 1280x800 MJPEG mode are unchanged.
- Still 122 fps with the MJPEG straight-to-gray hardware decode.

**Gain, to test:**
- The `Gain` quirk makes the Input tab show a **Camera Gain** slider (the pipelines got the
  default, 75).
- Our cameras' firmware reports no `gain` control (`v4l2-ctl -l`), so setting it is skipped
  (`softSet`) and the slider probably does nothing.
- Check by moving it and watching the image and the decision margin.

## Patches 22–25 and other picks from upstream (2026-09-24)

| Upstream | What we did |
|---|---|
| [#2437](https://github.com/PhotonVision/photonvision/pull/2437) 100-snapshot minimum | Applied as is (`photonvision-23`) |
| [#2149](https://github.com/PhotonVision/photonvision/pull/2149) snapshot loop (open) | Adapted: a separate **Auto Snapshots** toggle, so **Take Snapshot** still works by hand (`photonvision-23`) |
| [#2479](https://github.com/PhotonVision/photonvision/pull/2479) board sizes in mm | Ported (`photonvision-23`). #2480, which stores native units in the backend, isn't needed: the UI still sends inches |
| [#2477](https://github.com/PhotonVision/photonvision/pull/2477) exclude tags from PnP (draft) | Our own design instead: one list of bad tags for every camera, from the Settings page and robot code (`photonvision-22`). The draft had a per-pipeline switch for every tag |
| [#2138](https://github.com/PhotonVision/photonvision/pull/2138) `maxLineFitMSE` 2.5 (open) | A detector knob, still at 10 until tested (`08 --mse`) |
| [#2263](https://github.com/PhotonVision/photonvision/pull/2263) stream port forwarder | **Skipped.** The FRC radio passes only ports 1180–1190, and our maximum of 5 cameras (10 stream ports from 1181) fits. Sending all streams through one port would also make the browser's per-host connection limit worse |
| [#2493](https://github.com/PhotonVision/photonvision/pull/2493), [#2528](https://github.com/PhotonVision/photonvision/pull/2528) PhotonLib fixes | Robot side: workarounds in issue #10, since PhotonLib stays at alpha-2 |

## Upstream changes to test on the robot

- **Gateway (#2364):** static mode now uses `x.x.x.4` as the gateway (10.85.15.4, the VH-109
  radio's address). It doesn't matter on the robot network, which has no internet. In the shop the
  Wi-Fi default route still wins (lower metric).
- **NT reconnect (#2398):** upstream removed its 5-second "restart the NT client" workaround;
  ntcore does the reconnecting now. **Test on the SystemCore:** Jetson booting before the robot,
  robot reboot, and Ethernet unplugged and replugged.
- **Exposure order (#2429):** by the code, manual exposure ends in the same state. Check that
  exposure 50 survives a reboot.
- **Unchanged:** camera matching by USB path, calibration storage, and the settings DB schema.

## Not verified

- A camera the robot had disabled should be disabled again after a Jetson restart (the robot's
  retained `enabledRequest=false` arrives on reconnect). This is reasoned from NT4, not tested.
- The patch-11 unit tests didn't run on the laptop (they need a C++ compiler to build the x86
  JNI, and installing one needs sudo). They run on the Jetson in practice.
- The jar's version string is still `dev-v2026.1.1-27-gd8c9e8e1`: the build script applies
  patches on d8c9e8e, so `git describe` doesn't change. Tell builds apart by sha256.

## Upstream review, 2026-09-29

Everything merged after #2528, and open issues updated in 2026 that match our setup.

- **Don't take anything from `main` after #2566 (2026-09-17, WPILib alpha-7)** while the robot is on
  alpha-6 with photonlib alpha-2. It changes `PhotonPipelineMetadata` to nanoseconds (hash
  `ac0a45f6…` → `5231116d…`) and renames the time-sync topics (`offset_us` → `offset_ns`). The
  coprocessor and photonlib have to move together.
- **Ported:** #2617 (`currentVideoFormat` can be undefined while a camera activates; tabs vanished),
  as `photonvision-42`, adapted to our base plus two more unguarded spots (the stream's aspect
  ratio, the 3D view). #2352 (a disabled config reactivated onto a device already in use; the delete
  dialog named the wrong camera; identical cameras' cards all titled "Thrifty:"), as
  `photonvision-44`.
- **Already had:** #2629 (release a frame dropped because the pipeline changed mid-grab):
  `photonvision-11` has had that release since 2026-09-24. Its crop half doesn't apply: our base
  has no static crop.
- **Kept as notes:**
  - [#2443](https://github.com/PhotonVision/photonvision/issues/2443): `setPipelineIndex` from robot
    code goes from ~50 ms to up to 9.5 s after a few switches (2 OV9281s, 2026.3.2; no root cause).
    Only matters if robot code switches pipelines in a match; test it on the robot first if so.
  - [#2613](https://github.com/PhotonVision/photonvision/issues/2613) /
    [#2628](https://github.com/PhotonVision/photonvision/issues/2628): 90° image rotation breaks the
    multi-tag pose (0.3–0.4 m), confirmed on the libcamera path. If a camera is ever mounted
    rotated, compare our multi-tag pose with a solvePnP on the published corners.
  - [#1946](https://github.com/PhotonVision/photonvision/issues/1946): an OV9281 dropping off USB for
    8–10 s about every two hours (ESD or cabling). Our watchdog now really reopens a stuck camera
    (`photonvision-43`); a camera that drops off USB comes back through cscore's own reconnect once
    the kernel re-enumerates it.
  - [#2614](https://github.com/PhotonVision/photonvision/issues/2614): every UI change saves at
    once, no undo. A README note (duplicate the pipeline before experimenting; settings from any
    match are in the robot log and Rewind).
  - #2574 and #2344 (mrcal corner IDs, calibration uncertainty plots): watch.
  - #2586 / #2351 (virtual cameras, a combined tag and object-detection pipeline): watch, for the
    2027 combined camera.
- **Ours that upstream doesn't have yet** (not sent: ask Allen first): the S_FMT EBUSY camera stuck
  at a low resolution and its `kForceClose` recovery that never reopens (`-37`, `-43`), the
  startup `ConcurrentModificationException` that kills a camera thread (`-38`), the inverted
  "Failed to set video mode" log (`-37`), other dashboards never getting setting changes (`-41`),
  and the extra camera control sliders (`-39`). Open upstream PR #2633 does our direct gray decode,
  but turns off frame blocking in gray mode, which could hand out repeated frames.

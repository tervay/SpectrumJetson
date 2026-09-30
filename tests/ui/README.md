# PhotonVision browser tests

Playwright tests that drive the real dashboard in Chrome, on the laptop, against the Jetson's live
PhotonVision and cameras. They catch the bugs where the page, the backend and other dashboards
disagree, like the gamma slider that changed the camera but snapped back on screen
(`photonvision-39`) and the second dashboard that never saw changes (`photonvision-41`).

```bash
tests/ui/run.sh
```

- **Takes** a few minutes. Prints one line per control, with the backend setting each one moved.
- **Needs** the Jetson on the USB link or Wi-Fi (`run.sh` opens the SSH tunnel), and a
  PhotonVision built with `photonvision-45` or later.
- **Touches nothing you set up.** Each test copies the camera's current pipeline to a temporary
  `zz-uitest`, works on that, deletes it, and switches back. A run that was killed half way leaves
  `zz-uitest` behind; the next run deletes it first. Nothing is reset.
- **Refuses to run while the Jetson is connected to a robot** (`PV_UI_TEST_ON_ROBOT=1` overrides,
  for a robot that's disabled and safe).
- **Report:** `tests/ui/report/index.html` (screenshots and a trace of anything that failed).

| Setting | Default | |
|---|---|---|
| `PV_UI_CAMERA` | TopLeft, or the first camera | camera to test on |
| `PV_UI_HEADED=1` | off | show the browser |
| `PV_UI_TEST_VIDEO_MODES=1` | off | also switch resolutions (it reopens the camera) |
| `PV_URL` | `http://localhost:5800` | another PhotonVision |
| `JETSON_HOSTS` | `192.168.55.1 10.100.0.194` | where `run.sh` looks for the Jetson |

## The tests

- **`round-trip`:** every slider, switch, select, range and radio on every tab. For each one it
  changes the value the way a person would (the arrow buttons for sliders), then checks the page
  shows it, the backend changed (`/api/spectrum/uiState`), and a second dashboard shows it. Then it
  puts the value back and checks all three again.
- **`mask`:** the mask editor: draw a box on the stream, resize it from two corners, move it, add
  a second, Delete one, Remove all. Each step checked against the backend and a second dashboard.
- **`pipelines`:** Start from a copy (exact on the same camera), Create on every camera from
  another camera's pipeline (settings copied, each camera's orientation kept, other cameras left
  on their pipelines), and Switch every camera to pipeline N (cameras without N stay put). Creates
  `zz-uitest-copy` and `zz-uitest-all`, and deletes them from every camera afterwards. Also checks
  the Pipeline dropdown's "N: name" numbers match PhotonVision's.

- **`streams`:** a hidden tab (another tab in front, or minimised) disconnects its streams, and they
  come back when shown. Skipped if another dashboard is watching the camera.

- **`idle`:** starts the fake robot on the Jetson (`tests/fake-robot`, over SSH), then checks the
  dashboard's idle notice and ~30 FPS while it's disabled, the Full speed button, and the Settings
  switch. Skipped without SSH to the Jetson.

- **`event`:** the event pipeline when the field connects: a profile on every camera, the Settings
  card, the fake robot with the FMS attached, a dashboard override that sticks, and Switch now.

- **`ready`:** the Match Ready page: its verdict matches the health check, a live tile per camera,
  Check again.

- **`snapshots`:** save a named snapshot, add a pipeline, restore the snapshot through the card:
  PhotonVision restarts with every camera as before, and the automatic "Before restoring" snapshot
  has the change. Test snapshots are deleted.

- **`camera-controls`:** the Input tab's tag contrast readout matches `/api/tagContrast`, and there's
  no Camera Gain slider on a camera without gain.

- **`far-search`:** Settings > Robot state's far-tag search: its status, and no full-size searches
  while it's switched off.

**Limits:** 10 s per click, 2 min per test, 8 min per run; `run.sh` stops anything past 10 min. The
run fails if a camera ends on a different pipeline than it started on, or a `zz-uitest` pipeline is
left behind (or a `zz-uitest` snapshot), and says which.

## Writing a test

- Find controls by their label: `dash.control("slider", "Gamma", card)`. The shared components
  carry `data-pv-control` and `data-pv-label` (`photonvision-45`), so tests don't depend on
  Vuetify's generated ids.
- Check the backend with `cameraState(request, camera)`, not only the page.
- Use the `pipeline` fixture to work on the temporary pipeline, and put back anything outside it.
- New features get a test in `specs/` as they're built.

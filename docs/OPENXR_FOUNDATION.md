# OpenXR VR foundation

This worktree contains an optional Windows OpenXR vertical slice for `racers-portable`. The default build remains desktop-only. The VR path replays the race world twice using independent runtime poses and asymmetric FOVs; it does not duplicate or distort the final desktop frame.

## Local development layout

This is the exact layout used for the first validated build on 2026-08-19:

```text
C:\Users\Ryan\Downloads\legoracers\
|-- LegoRacers.iso                 owned source media; never modified
|-- racers-portable\               clean master baseline at 38edef2f
|-- racers-portable-openxr\        this worktree, branch feature/openxr-vr-slice
`-- retail-data\
    |-- .gitignore                 deny-all safeguard
    |-- game\                      flat, local-only runtime assets
    |-- installer\                 local InstallShield cabinet staging
    |-- manifests\                 hashes, extraction tests, and smoke evidence
    `-- tools\                     local unshield build and licenses
```

`retail-data/game` contains exactly the 54 runtime files named by `LEGORacers/emscripten/filesystem.cpp`: `LEGO.JAM`, 50 `TUN` files, and 3 intro `AVI` files. Retail data stays outside every Git repository and must never be packaged or redistributed.

The ISO is 202,573,824 bytes with SHA-256 `C66A0405EFFB67C848AE813FB5AE6F3612AD7140C07633240E6C2232B17CC13D`. It was inventoried and extracted read-only; its final hash is unchanged.

## Reproducible builds

Prerequisites used by the validated build:

- CMake 4.3.1
- Visual Studio 2022 x64 generator
- MSBuild 17.14.23
- MSVC 19.44.35216, tools 14.44.35207
- Windows SDK 10.0.26100.0
- initialized miniaudio submodule at `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d`

Desktop-compatible build (OpenXR stub):

```powershell
cmake -S . -B build-openxr-off -G "Visual Studio 17 2022" -A x64 -DRACERS_OPENXR=OFF
cmake --build build-openxr-off --config RelWithDebInfo --parallel
```

OpenXR build:

```powershell
cmake -S . -B build-openxr-on -G "Visual Studio 17 2022" -A x64 -DRACERS_OPENXR=ON
cmake --build build-openxr-on --config RelWithDebInfo --parallel
```

The OpenXR build fetches the official Khronos OpenXR-SDK 1.1.60 source at exact commit `64f2b37c8c6da3d83c9b4d11865ba1fb752cb8ec`. It statically links the loader; no `openxr_loader.dll` is required beside the game.

Run desktop:

```powershell
.\build-openxr-off\RelWithDebInfo\LEGORacers.exe `
  --renderer opengl3 `
  --path C:\Users\Ryan\Downloads\legoracers\retail-data\game `
  -window -novideo
```

Run OpenXR:

```powershell
.\build-openxr-on\RelWithDebInfo\LEGORacers.exe `
  --openxr `
  --renderer opengl3 `
  --path C:\Users\Ryan\Downloads\legoracers\retail-data\game `
  -window -novideo
```

Options:

- `--openxr` or `--vr`: request VR. An OpenXR-off build reports a clear error.
- `--vr-world-scale <n>`: game units per physical meter; default `10`, valid `(0, 1000]`.
- `--vr-seated-height <n>`: vertical camera-anchor offset in meters; default `0`, valid `[-3, 3]`.
- `F12`: request recenter while racing.

## Architecture and seams

The game CPU-transforms world geometry into screen-space `D3DTLVERTEX` vertices inside `GolDP/src/render/gold3drenderdevice.cpp`; `miniwin/src/d3d/d3ddevice.cpp::DrawIndexedPrimitive` receives those final vertices. Stereo therefore has to occur before that transform.

The vertical slice uses these boundaries:

- `LEGORacers/src/race/racesession.cpp::RaceSession::Run` chooses stereo or the unchanged desktop `Draw` path.
- `RaceSession::DrawOpenXR` saves the racing-camera anchor, obtains two runtime views, applies each relative 6DOF pose plus IPD and asymmetric FOV to `GolCamera`, and replays only the world pass for each eye.
- `GolDP/src/camera/golcamera.cpp::GolCamera::BuildProjection` is the projection seam. Near clip defaults to 0.05 m converted through world scale; the existing far plane is retained.
- `LEGORacers/src/race/racecameracontroller.cpp::RaceCameraController::Update` remains the vehicle-relative anchor. Head tracking is applied after that controller update and the original transform is restored after submission.
- `vr/src/racers_vr_openxr.cpp` owns all OpenXR handles, actions, frame pacing, swapchains, WGL binding, gravity-preserving yaw recenter, and private per-eye FBO/depth targets. `vr/include/racers_vr.h` exposes a small POD/C boundary without OpenXR types.
- `miniwin/src/d3d/backends/backends.cpp` shares the selected backend through SDL's process-global property store. This fixes the prior EXE/GolDP static-library split where `--renderer opengl3` affected only the EXE copy.
- `LEGORacers/src/race/playercontrols.cpp` adds XR input through the existing steering and button handlers. Keyboard/gamepad bindings and XR actions are combined as logical-OR sources, while centered XR steering cannot override a stronger desktop/touch axis.
- `LEGORacers/src/app/win32golapp.cpp::Tick` handles XR lifecycle/actions and maps pause through the existing Escape event route.

OpenGL is the first backend because the existing game renderer already owns a current WGL HDC/HGLRC and its normal triangle draws do not rebind the scene FBO. The XR layer directly attaches runtime OpenGL swapchain textures. OpenXR runs request a 4.5 core context so common Windows runtime minimums can be met, while ordinary desktop OpenGL runs retain the upstream 3.3 request. Calls that rebind miniwin's scene target (`Present`, resize, or viewport recreation) must remain outside `BeginEye`/`EndEye`.

Actions are suggested for Oculus Touch, Valve Index, Microsoft motion controllers, and the Khronos simple controller. Current mappings are thumbstick steering, right trigger throttle, left trigger brake, right-face/select power-up, left menu/B pause, right thumbstick recenter, and both-hand haptics. Session/focus loss clears held race inputs; countdown/pause gating replays still-held buttons when control returns.

When OpenXR is requested, desktop window focus loss does not enter the legacy blocking/minimized loop. This is required even before the lazy race-session initialization so runtime events and frame cadence cannot be starved by the companion window yielding focus to a compositor.

## Validation completed

- Clean unmodified baseline built x64 and reached live Single Race gameplay with owned assets.
- OpenXR-off builds succeeded and post-review smokes showed the animated menu using actual OpenGL 3.3; no asset, watchdog, renderer, or stall errors.
- OpenXR-on builds succeeded with the static OpenXR loader.
- The installed Virtual Desktop runtime loaded, its compatibility layer loaded, and `xrCreateInstance` succeeded. With no headset available, `xrGetSystem` returned `XR_ERROR_FORM_FACTOR_UNAVAILABLE`; the program destroyed the instance/runtime cleanly and continued into live desktop gameplay through the OpenGL fallback.
- Both default desktop and optional OpenXR source paths are buildable. No commit, push, or PR was made.

Local evidence is under `C:\Users\Ryan\Downloads\legoracers\retail-data\manifests` (`baseline-smoke`, `feature-desktop-smoke`, and `openxr-runtime-smoke`).

## Known limits and next headset test

Actual headset stereo, pose direction, IPD, controller bindings, and haptics have not yet been observed because the active runtime exposed no HMD. Connect the headset/start the selected runtime, rerun the OpenXR command, and verify the `OpenXR: initialized stereo OpenGL session` log plus tracked left/right views before tuning world scale or the camera anchor.

This first slice is deliberately single-player racing only. Demo/split-screen and exclusive-draw power-up frames fall back to the desktop render. HUD and menus are not submitted to XR yet, and the desktop mirror is not a dedicated spectator pass while stereo is active. Runtime loss falls back safely but reinitialization is deferred until the next race. Simultaneous touch-screen and XR race-button releases still share the legacy flags; keyboard/gamepad and XR arbitration is implemented.

The next UI step is a stable composition-layer or world quad containing the original 640x480 menu/pause compositor. Intersect an XR controller ray with the quad, convert UV to original cursor coordinates, and feed motion/click/back through `Win32GolApp::UpdateMousePosition`, `OnCursorMoved`, and the existing DirectInput mouse-button injection. This keeps original widgets and drag behavior intact without head-locking the race world.

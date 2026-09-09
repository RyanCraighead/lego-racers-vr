# LEGO Racers VR — PC VR Mod

**LEGO Racers (1999), in VR on your PC.**

An unofficial PC VR mod by **Ryan Craighead**, adding OpenXR stereo rendering, head tracking, and VR controller input to LEGO Racers.

This is the **Windows PC VR version**. The [standalone Quest app](https://github.com/RyanCraighead/quest-racers) is a separate project. Using a Quest headset with this version still requires a PC connection; this repository is not an Android APK.

## What the mod adds

- Stereo 3D racing, with a separate view rendered for each eye.
- Six-degree-of-freedom head tracking: look around and lean relative to the game's racing camera.
- VR controller steering, acceleration, braking, power-ups, pause, and recentering.
- Adjustable world scale and seated camera-height offset.
- Optional VR: launch without `--openxr` to play on the desktop.

The mod uses the original game's content. **You must supply your own installed copy of LEGO Racers. Game data is not included.**

## Current status

The current PC VR target is **Windows x64, OpenXR, and single-player racing**. This is a development build, not a claim that every headset or runtime combination has been tested.

Important limitations in the current `main` branch:

- The headset receives the **3D race world**. Menus, the race HUD, and pause overlays currently remain on the desktop, so keep a desktop view and keyboard or gamepad available.
- Split-screen and demo/attract-mode races do not use the OpenXR race view.
- Steering uses controller thumbsticks, not a grab-and-turn virtual steering wheel.
- Drift and camera-change actions do not yet have dedicated OpenXR bindings; use the game's existing keyboard/gamepad controls.
- The upstream engine's Linux, macOS, and web support does **not** mean this PC VR integration supports those platforms.

## What you need

- A Windows x64 PC and graphics drivers supporting OpenGL 4.5. The VR path uses the `opengl3` renderer and requests an OpenGL 4.5 context.
- A PC-connected VR headset and an active Windows OpenXR runtime.
- An OpenXR-enabled build of this mod.
- The installed LEGO Racers game folder:
  - `LEGO.JAM` — game content.
  - `*.TUN` — music files; keep these alongside the game content.
  - `*.AVI` — original intro videos, if you want to play them. The launch example below skips them.

Use the **installed game folder**, not the installer folder on a mounted CD/ISO. You can leave your original installation where it is and point the mod at it.

## Install and launch

1. Check this repository's [Releases](https://github.com/RyanCraighead/lego-racers-vr/releases) for a packaged **Windows PC VR build**. If no Windows package is listed, use the source-build instructions below.
2. Extract the complete package into its own folder. Keep `LEGORacers.exe` and its accompanying DLLs together; do not copy only the executable.
3. Connect your headset to the PC and select the intended OpenXR runtime through your headset software.
4. Open PowerShell in the mod's folder and run this **single-line command**, replacing the game-data path with your own:

```powershell
.\LEGORacers.exe --openxr --renderer opengl3 --path "C:\Games\LEGO Racers" -novideo -window
```

5. Use the desktop menus to start a single-player race. The stereo VR view starts during the race; the desktop menu is not currently a headset menu.
6. Sit comfortably, look forward, and recenter using the control below.

You do not need to replace the original game's executable. Back up your saves before experimenting with development builds.

## VR controls

Default **Oculus Touch interaction-profile** bindings:

| Action | Control |
| --- | --- |
| Steer | Left or right thumbstick, horizontal |
| Accelerate | Right trigger |
| Brake | Left trigger |
| Use power-up | Right A button |
| Pause | Left menu button |
| Recenter | Click the right thumbstick |

For the Valve Index profile, pause is bound to the **left B button**. For the Microsoft motion-controller profile, the power-up action uses the **right grip/squeeze button**. These are bindings implemented in the source, not a tested-headset compatibility list; runtime remapping can also affect them.

## Launch options

| Option | Purpose |
| --- | --- |
| `--openxr` or `--vr` | Enable the OpenXR race view. Requires a build made with `RACERS_OPENXR=ON`. |
| `--path "directory"` | Read your game files from that folder. |
| `--vr-world-scale 10` | Game units per metre; default is `10`. |
| `--vr-seated-height 0` | Upward camera-anchor offset in metres; default is `0`. |
| `--renderer opengl3` | Select the OpenGL renderer. VR mode also selects it automatically. |
| `-novideo` | Skip the original intro videos. |
| `-window` | Keep the desktop game window windowed. |
| `--help` | Show the available command-line options. |

## Build from source

Install **Git**, **CMake 3.25 or newer**, and **Visual Studio 2022 / Build Tools** with the **Desktop development with C++** workload and a Windows SDK.

Run these commands in PowerShell:

```powershell
git clone https://github.com/RyanCraighead/lego-racers-vr.git
cd lego-racers-vr
cmake -S . -B build-vr -G "Visual Studio 17 2022" -A x64 -DRACERS_OPENXR=ON
cmake --build build-vr --config RelWithDebInfo --parallel
```

The first configuration downloads the pinned SDL3 and OpenXR SDK dependencies, so it needs an internet connection. A separate system-wide OpenXR SDK installation is not required.

With the Visual Studio configuration above, launch the built application with:

```powershell
.\build-vr\RelWithDebInfo\LEGORacers.exe --openxr --renderer opengl3 --path "C:\Games\LEGO Racers" -novideo -window
```

**Do not omit `-DRACERS_OPENXR=ON`: VR is disabled by default in the CMake configuration.** Omitting the launch option `--openxr` runs the desktop version instead.

## Troubleshooting and feedback

- **“Configured without -DRACERS_OPENXR=ON”:** use a PC VR build or reconfigure and rebuild with that option enabled.
- **No headset view:** confirm the headset is connected to the PC, the intended OpenXR runtime is active, and a single-player race has started. Desktop menus and demo races are not the VR race view.
- **Missing music:** check that the original `*.TUN` files are present in the folder passed to `--path`, and check the game's music volume and PC audio output.
- **Missing DLL error:** extract the entire Windows package, keeping its runtime files together.
- **View is offset:** recenter while seated and looking forward; adjust `--vr-seated-height` if needed.

[Report an issue](https://github.com/RyanCraighead/lego-racers-vr/issues) with your mod version or commit, headset, OpenXR runtime, GPU/driver, launch command, and steps to reproduce. Include relevant error messages, but do not upload the original game data.

## Credits and licensing



This mod builds on [racers-portable](https://github.com/isledecomp/racers-portable) and the [LEGO Racers decompilation project](https://github.com/isledecomp/racers). Their engine and portability work make this adaptation possible. The project also uses SDL3, OpenXR, miniaudio, and the existing vendored video-decoding components.

See [LICENSE](LICENSE), individual source headers, and the notices accompanying third-party components for their licensing terms. Original game assets are not supplied by this project.

This is an unofficial fan project, not an official LEGO release.

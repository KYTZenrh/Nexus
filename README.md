# Nexus

A custom game dashboard for the PS5, installed as a native app (title ID `PPSA99784`). It shows
your installed games and apps with their real icons, launches them, and has Trophies, System and
Settings tabs, with synthesized sound effects and background music. Everything is drawn by its own
software renderer at 60 fps.

Version 1.0.0. Built for PS5 firmware 13.60 running Byepervisor Reborn + HyperHEN.

## Install on the console

1. Run the exploit chain (HyperHEN, with ShadowMountPlus and the ELF loader on port 9021).
2. Copy the `PPSA99784` folder (from a release zip, or `build/ps5native/PPSA99784` after a build) to
   `/data/homebrew/PPSA99784` with PS5Upload or FTP. ShadowMountPlus adds Nexus to the home screen
   within about 30 seconds.
3. In `/data/HyperHEN/config.ini`, under `[app_jailbreak]`, add `PPSA99784` to `exact_title_ids`.
   Nexus needs this to read the game library, storage and trophy totals.
4. Open Nexus from the home screen.

Works with the DualSense and with PS4-type controllers (Nexus runs as a media app, the kind of app
the PS5 lets PS4 controllers use). PS4 controllers do not vibrate outside PS4 games.

## Build

The PS5 app is built inside WSL (Ubuntu) with the
[ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk) and the tools in
`third_party/ps5-native-app-boilerplate`:

```
wsl -d Ubuntu -- bash tools/build-ps5-native.sh            # build into build/ps5native/PPSA99784
wsl -d Ubuntu -- bash tools/build-ps5-native.sh --upload   # build and copy to the console over FTP
```

Close Nexus on the console before uploading (a running app's files cannot be replaced).

PC preview (Windows, llvm-mingw + SDL2; paths are set at the top of the script):

```
powershell -ExecutionPolicy Bypass -File tools\build-pc.ps1
build\pc\nexus.exe --preview
```

`--preview` shows clearly labelled sample games and trophies; `--icons <folder>` gives the sample
games real covers (`1.png`, `2.png`, ...). Keys: arrows/WASD move, Enter confirm, Esc back, Q/E =
L1/R1, T = Triangle, F11 fullscreen. A controller works too.

## How it works on the console

- **Library:** read from the console's own title database (`/system_data/priv/mms/app.db`) by a
  small built-in SQLite reader (`src/core/sqlite_read.cpp`).
- **Sandbox:** Nexus asks HyperHEN to lift its sandbox by writing a request file to its own
  `/download0` folder (`requestJailbreak` in `src/platform/ps5/ps5_platform.cpp`).
- **Launching:** apps may not call the system launch functions, so Nexus sends a small payload
  (`tools/ps5-launcher/launcher.c`, shipped as `assets/launcher.elf`) to the ELF loader. It closes
  Nexus and opens the game's deep link so the home screen starts it.
- **Trophies:** apps without a trophy set show 0 of 0; the System tab shows your account totals from
  the console's trophy summary. Per-game progress for PS4 games is not read.
- **Logs:** `/data/nexus_log.txt` (start-up) and `/data/nexus_launcher.log` (launches).

## Layout

```
src/ui/                 screens, drawing, animation (shared by PC and PS5)
src/core/               sound synthesis, mixer, SQLite reader
src/platform/           Platform interface + one folder per target (pc, ps5)
assets/                 fonts (Barlow, SIL OFL), trophy icons, app icon
tools/                  build scripts and console helpers (tools/ps5-launcher)
third_party/            stb (public domain/MIT), ps5-native-app-boilerplate (GPL-3.0-or-later)
docs/ARCHITECTURE.md    design notes
```

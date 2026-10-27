# Nexus — Architecture & Feature Status

Target: PS4 FPKG running on PS5 firmware 13.60 via Bypervisor + HyperHEN (user-supplied environment).
Nothing here is claimed to work on console until the user has tested it. Status values:
**Unverified** (researched, not built) · **Built** (compiles/runs on PC only) · **Verified** (user-tested on console) · **Unsupported**.

## Research findings (source: Itemzflow, GPL-3.0, read as reference only)

| Question | Finding |
|---|---|
| UI technology | Itemzflow is native C++ with an OpenGL ES 2 renderer built on the OrbisDev SDK. No embedded web view was found, so **a web (HTML/JS) UI is not a safe plan**. Nexus will be native C++. |
| Launching a game | In-process: load `/system/common/lib/libSceSystemService.sprx`, get the foreground user (`sceUserServiceGetForegroundUser`), call `sceLncUtilLaunchApp(titleId, NULL, &param)` (Itemzflow `utils.cpp` `Launch_App`). No helper is needed for the launch call itself. The process must have the privileges the exploit environment grants. |
| Listing installed games | Itemzflow reads the SQLite DB `/system_data/priv/mms/app.db` (table `tbl_appbrowse_0<userId>`). Needs filesystem privileges beyond a normal app. |
| Helper | Itemzflow ships a daemon (`ITEM00002`) for background jobs (installs, etc.), **not** for launching. Nexus needs no helper for launch or the game list. A helper is only justified for work that must outlive the UI or needs extra privilege. |
| System info | `sceKernelGetCpuTemperature` is used by Itemzflow. Other values are to be researched one at a time. |
| Trophies | Itemzflow does not display trophy data. Real trophy data is the least-researched item and may be unsupported. |

## Architecture

Single C++ codebase, three layers, so the UI can be previewed on PC:

```
ui/        screens, shelf, animations, themes  (pure C++, no platform calls)
core/      state, input mapping, feature registry with status flags
platform/  one interface, several implementations:
             pc/   SDL window + input + audio; sample data ONLY in an explicit --preview mode
             ps4/  ABANDONED (see below) — kept only as a record of what didn't work
             ps5/  current console target: native PS5 app, ps5-payload-dev SDK
```

### Console target: abandoned PS4/OpenOrbis, now native PS5 (2026-10-26)

The first console attempt built Nexus as a PS4 FPKG with the OpenOrbis toolchain. On the user's
console (PS5 fw 13.60, Byepervisor Reborn + HyperHEN) this caused a hard kernel panic — and so did
the OpenOrbis toolchain's own unmodified sample app and a third-party tool (ORBISPatches). Meanwhile
the user's own working homebrew (Itemzflow (PS5), Homebrew Store, PS5-Xplorer) are native PS5 apps,
not PS4 FPKGs. Conclusion: PS4-FPKG-via-HyperHEN homebrew is not reliable on this setup; native PS5
apps are. Nexus was rebuilt from scratch as a native PS5 app using **ps5-payload-dev/sdk** (target
triple `x86_64-sie-ps5`), the same category of SDK real working apps on this console use. The
PS4 code is left in `src/platform/ps4/` purely as a record; it is not built or used.

Key differences from PS4 that caused real bugs when first tried naively:
- PS5 needs `sceSystemServiceHideSplashScreen()` before opening video, and the v2 buffer APIs
  (`sceVideoOutSetBufferAttribute2`, `sceVideoOutRegisterBuffers2`,
  `sceKernelAllocateMainDirectMemory`), not the PS4-era v1 ones.
- The PS5 GPU framebuffer is **tile-swizzled**, not linear — `src/platform/ps5/ps5_tilemap.*`
  (adapted, with attribution, from `ps5-payload-dev/SDL`'s own tiling code, zlib license) rearranges
  our plain pixel buffer into that layout before every flip.
- Sce libraries (`libSceVideoOut.so` etc.) are **dynamically linked**; the build must pass `-lSceVideoOut
  -lScePad -lSceUserService -lSceSystemService -lSceSysmodule` or the linker reports them undefined.
- The toolchain's prebuilt `crt1.o` only links cleanly against **LLVM 18** (not whatever is newest);
  newer lld reports "duplicate symbol" errors in it. LLVM must also live at a path with no spaces
  (`C:\llvm18`, not `C:\Program Files\LLVM`) or the linker search breaks on `x86_64-sie-ps5`.

Build: `tools\build-ps5.ps1` → `build\ps5\nexus.elf`. Deploy (`-Send`) streams the raw ELF to the
console's ELF loader on port 9021 (the user's own "Elf Loader") — no installer, no PKG, for this
first test. A full installed app with its own icon (modeled on the SDK's `install_app` sample,
which writes to `/system_ex/app/<ID>` and `/user/app/<ID>` with a `param.json` and `icon0.png`) is
a later step once the ephemeral build is proven stable.

### Rendering (decided while porting to the console)

The OpenOrbis toolchain ships SDL 2.0.x (no `SDL_RenderGeometry`) and no SDL_ttf, and its proven
display path is a CPU-written frame buffer flipped through `sceVideoOut`. So Nexus no longer draws
through SDL at all: `src/ui/gfx.cpp` is a small software renderer (triangles with per-vertex colour
and texture coordinates, alpha blending, anti-aliased edges, stb_truetype text, stb_image PNGs)
writing 0xAARRGGBB pixels into a buffer. The PC preview shows that buffer in an SDL window and the
console build copies it to the frame buffer, so both produce the same pixels. Measured on a PC:
about 13 ms per frame on Home; the launch zoom is the heaviest at about 35 ms.

## Console port stages (PS5-native)

| Stage | Contents | State |
|---|---|---|
| 1 | Ephemeral ELF payload; UI with sample data; ScePad input and rumble; staged boot with on-screen progress + `/data/nexus_log.txt`; `hello_world.elf` built as a near-zero-risk control test | Built; **not yet run on the console** |
| 2 | Audio (sfx + music) via `libSceAudioOut` | Not started |
| 3 | Install as a real app with its own icon (title dir under `/system_ex/app` + `/user/app`) | Not started |
| 4 | Real game list, launch, trophy data, system info | Not started |

- The shelf only ever shows games returned by `platform/` (nothing fake). On PC, `--preview` supplies clearly labeled sample entries; the console build has no preview mode.
- Every capability (`launch`, `game_list`, `trophies`, `sysinfo`, `vibration`) is a registry entry with a status. The UI reads it and shows Verified/Unverified/Unsupported instead of assuming.

## Feature status

| Feature | Status | Next step |
|---|---|---|
| Dashboard + game shelf UI | Unverified | Build on PC |
| Zoom/fade launch transition | Unverified | Build on PC |
| DualSense navigation | Unverified | Research PS4 pad API (works as DS4-compat on PS4 apps) |
| Vibration (default low-medium, adjustable) | Unverified | Test on console |
| Game list from app.db | Unverified | Test on console |
| Game launch (`sceLncUtilLaunchApp`) | Unverified | Test on console |
| System info | Unverified | Research API per value |
| Trophy tab UI (filters, red cushion + gold edge design) | Unverified | Build on PC |
| Real trophy data | Unverified | Research feasibility |
| KYT Helper | Not needed yet | Revisit only if a real need appears |

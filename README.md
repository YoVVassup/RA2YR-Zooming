# ViewCtrl — Zoom Plugin for C&C: Yuri's Revenge

[![License: GPL v3](https://img.shields.io/github/license/YoVVassup/RA2YR-Zooming)](LICENSE.txt)
[![Language](https://img.shields.io/badge/language-C%2B%2B-00599C?logo=cplusplus&logoColor=white)](#technical-implementation)
[![Platform](https://img.shields.io/badge/platform-Windows%20x86-0078D6?logo=windows&logoColor=white)](#requirements)
[![Game](https://img.shields.io/badge/game-C%26C%3A%20Yuri%27s%20Revenge-E6A23C)](#requirements)
[![Framework](https://img.shields.io/badge/framework-Syringe-7B68EE)](#technical-implementation)
[![Tests](https://img.shields.io/badge/tests-51%20cases%20%7C%20533%20assertions-4CAF50)](#status)
[![Fork of](https://img.shields.io/badge/fork%20of-ChoyoTsumu%2FRA2--about-1f6feb?logo=github&logoColor=white)](https://github.com/ChoyoTsumu/RA2-about)

---

## 🎮 Overview

A viewport zoom plugin for Command & Conquer: Yuri's Revenge. It magnifies the tactical view at the render level, keeps the point under the cursor fixed by driving the game camera, and un-magnifies coordinates exactly where the game turns screen positions into game space, so selection and clicks land on what is actually displayed.

Built on the **Windows API** with **Syringe** (the injection framework used by Ares, Phobos and other YR extensions) for injection and lifecycle management. Only code hooks — the game executable and its resources are never patched on disk.

---

## ✨ Features

- 🔍 **Ctrl + mouse wheel** zoom over the tactical view: gear 1.15 per step, range 1.0x–4.0x, smooth frame-independent interpolation
- 🎯 The point under the cursor stays fixed: the game camera moves with every zoom step so nothing slips away
- ↩️ **Double press Ctrl** (within 400 ms) resets the zoom to 1.0x; the accumulated camera offset is undone automatically whenever the zoom returns to 1.0x
- 🧭 **Arrow keys** pan the view while zoomed
- 🖼 Render-level magnification: the view rect is backed up and upscaled (nearest neighbour) over the frame the game has already drawn
- 🧰 Only the tactical view is magnified — sidebar, HUD and the physical cursor are never remapped
- 🧮 Coordinates are transformed only where the game converts them (tactical clicks, selection rubber band, right drag)
- 📦 No GScript dependency: the zoom factor lives entirely in ViewCtrl — and when a mod ships its own scaler, ViewCtrl yields to it instead of competing (see Status)
- ⚡ Minimal performance overhead

---

## ✅ Status

- 🧪 **Unit tests:** 51 test cases / 533 assertions — `tests\bin\ViewCtrlTests.exe`
- 📊 **Line coverage:** ~99% of `src/` (measured with OpenCppCoverage over the doctest suite; the remainder is `GameCamera::Enable`'s success path, which needs the real game module, plus a few defensive error branches)
- ⚠️ **Competing scalers auto-detected:** when the mod scales the view itself — `GScript.ext` (e.g. Tiberium Crisis) or Telescope v1.4 (which bundles these same nine hooks) — `GameInt` notices (module file name, or any other loaded plugin claiming ≥3 of our nine hook sites) and keeps ViewCtrl inert: no init thread, no window subclass, every hook replays the original instructions, so the mod's own scaler runs undisturbed
- 🔬 **Static audit:** every hook site was verified against the YR Hook Encyclopedia — Syringe-stub safe (no relative branches inside the stolen windows, resume addresses on instruction boundaries), no registry conflicts; the bootstrap hook `0x52CAE9` is shared with Ares (`_YR_PostGameInit`) and composes with it
- 🖥 **Render order verified by disassembly:** magnification runs after the game draws into `DSurface::Composite`, before the cursor blit and before `Frame::Present` — the magnified pixels are what reaches the screen
- 🎮 **Verified in-game** on Yuri's Revenge 1.001 (modded install, 1920x1080): zoom range and anchor, click/selection transforms, reset and undo

---

## 💻 Requirements

- OS: Windows 7 or later (32-bit process)
- Game version: Yuri's Revenge 1.001
- Runtime: Syringe injection framework to load the DLL

---

## 📥 Installation

1. Make sure the game runs through Syringe (`Syringe.exe` or a mod launcher based on it)
2. Place the compiled DLL in the game root directory
3. Register the DLL in your Syringe loader configuration
4. Launch the game — Release builds run silently and write no files; a Debug build creates a `ViewCtrl.log` next to the game executable for troubleshooting

---

## 🔨 Building

`YRpp` (the Syringe headers) is a git submodule, so clone recursively:

```bash
git clone --recurse-submodules https://github.com/YoVVassup/RA2YR-Zooming.git
```

If you already have a clone, initialise the submodule once:

```bash
git submodule update --init
```

Then build `Release|Win32` (v143 toolset):

```bash
msbuild viewctrl.vcxproj /p:Configuration=Release /p:Platform=Win32
```

Tests live in `tests\test.vcxproj` and build as `Debug|Win32` only:

```bash
msbuild tests\test.vcxproj /p:Configuration=Debug /p:Platform=Win32
tests\bin\ViewCtrlTests.exe
```

---

## 🎛 Controls

| Action | Effect |
|--------|--------|
| **Ctrl + scroll wheel up/down** | Zoom in / out (x1.15 per step, clamped to 1.0x–4.0x) |
| **Double press Ctrl** (within 400 ms) | Reset the zoom to 1.0x and undo the camera offset |
| **Arrow keys** (while zoomed) | Pan the view |

Without Ctrl the wheel is passed to the game untouched. All zoom parameters are compile-time constants (`src\Zoomer.hpp`). To adjust them, modify the source code and recompile.

---

## 🤝 Compatibility

- Hooks only, never rewrites the executable — co-loadable with other Syringe DLLs (Ares, Phobos, ...); Syringe chains multiple DLLs at shared sites
- `0x52CAE9` shares its 5 bytes with Ares `_YR_PostGameInit`; ViewCtrl runs the original bytes afterwards, so both handlers compose
- Earlier builds could delegate the zoom factor to GScript; ViewCtrl no longer reads or writes GScript — if GScript's own zoom feature is active, disable one of the two to avoid double zooming
- If you encounter compatibility issues with a specific mod, please open an Issue with details and the `ViewCtrl.log` from a Debug build (Release builds compile logging out entirely)

---

## ⚙️ Technical Implementation

- **Injection framework:** Syringe `DEFINE_HOOK` — 9 hooks, all addresses centralized in `src\GameAddrs.hpp` with source annotations (YRpp / ReSource / Encyclopedia)

| Site | Bytes | Purpose |
|------|-------|---------|
| `0x52CAE9` | 5 | bootstrap after game init (`InitGame`) — Ares-shared, composes |
| `0x4F44AF` | 5 | pre-render: restore the clean view backup, tick the zoom lerp |
| `0x4F451B` | 5 | post-render: magnify the drawn view |
| `0x6D864E` / `0x6D868A` | 6 | `ClampCoordMap` — view width/height reads clamped to the magnified source rect (falls back to the last good size) |
| `0x692325` | 6 | `DisplayClass::ProcessClickCoords` — tactical click transform |
| `0x6D9F80` / `0x6D9FC0` | 6 | selection rubber band start/end transform |
| `0x693791` | 6 | `ScrollMapEdge` — right-drag scroll speed transform |

- **Render path (verified by disassembly):** `MainLoop` → `GScreenClass::Render` (`0x4F4480`) draws the frame into `DSurface::Composite` → ViewCtrl magnifies → `wwmouse->DrawCursorBuffered` blits the unscaled cursor → virtual `RenderFrame` → `Frame::Present`
- **Input:** subclasses the game window procedure (`SetWindowLongPtrW`); the wheel zooms only while Ctrl is held and the cursor is over the tactical view, everything else is forwarded to the game; the cursor position itself is never remapped
- **Camera:** every zoom step moves the game camera (`TacticalMapClass::SetCameraPosition`, `0x6D6000`) so the focused point stays under the cursor; the offset is undone at 1.0x
- **View rect:** `DSurface::ViewBounds`, polled at most every 250 ms and accepted only while nothing is magnified; the rect itself excludes the sidebar
- **Coordinate transforms:** the points where the game turns screen positions into game space are un-magnified; `Zoomer::UnMagnify` is the exact inverse of the upscale the renderer draws

---

## 🏅 Credits

**Fork lineage**

- This repository is a fork of [ChoyoTsumu/RA2-about](https://github.com/ChoyoTsumu/RA2-about) — the original viewport zoom concept and core implementation by **ChoyuTsumu**, assisted by **Sovietianqi**
- This fork: [YoVVassup/RA2YR-Zooming](https://github.com/YoVVassup/RA2YR-Zooming) — reworked as a render-level Syringe plugin (camera driving, coordinate transforms, unit tests, static hook audit); maintained by **YoWassup**

**Acknowledgements — research material used to verify the logic**

- [YRpp](https://github.com/Phobos-developers/YRpp) — Syringe / C&C headers (git submodule of this project)
- [RA2YR_ReSource](https://github.com/Ritanlisa/RA2YR_ReSource) — reverse-engineered `gamemd.exe` symbol and class database used to name and cross-check addresses
- YR Hook Encyclopedia — registry of hook sites from Ares, Phobos, Kratos and others, used for conflict checking and the Syringe-safety audit
- [Ares](https://ares-developers.github.io/Ares-docs/) — the Syringe injection framework and reference extension; shares our bootstrap hook `0x52CAE9`
- [Phobos](https://github.com/Phobos-developers/Phobos), [Kratos](https://github.com/ra2diy/KratosPP) — hook-site cross-checks

Contributions and pull requests are welcome.

---

## 📜 License

This project is licensed under the GNU General Public License v3.0. See [LICENSE.txt](LICENSE.txt) for details.

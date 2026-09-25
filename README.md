# ViewCtrl — Free Zoom Plugin for C&C: Yuri's Revenge

---

## Overview

A viewport zoom enhancement plugin for Command & Conquer: Yuri's Revenge. It magnifies the tactical view at the game render level, keeps the point under the cursor fixed by driving the game camera, and un-magnifies coordinates at the places where the game turns screen positions into game space, so selection and clicks land on what is actually displayed.

Built on the **Windows API** with **Syringe** (the injection framework used by Ares) for injection and lifecycle management. Stable and compatible.

---

## Features

- Zoom the tactical view with **Ctrl + mouse wheel** (gear 1.15 per step, range 1.0x–4.0x, smooth interpolation)
- The point under the cursor stays fixed: the game camera moves with the zoom so nothing slips away
- **Double press Ctrl** resets the zoom to 1.0x
- **Arrow keys** pan the magnified view
- Render-level magnification: the view area is backed up and upscaled (nearest neighbour) over the drawn frame
- Coordinates are transformed only where the game converts them (tactical clicks, selection rubber band, right drag), the physical cursor is never remapped
- Optional **GScript/Ares** integration: when `GScript.ext` is loaded the zoom factor is delegated to it
- Does not modify the game executable or resources
- Minimal performance overhead

---

## Requirements

- OS: Windows 7 or later (x86)
- Game version: Yuri's Revenge 1.001
- Runtime: Syringe injection framework (Ares dependency)

---

## Installation

1. Make sure the game directory has Syringe configured (i.e. `Syringe.exe` exists or `gamemd.exe` is launched via Syringe)
2. Place the compiled DLL in the game root directory
3. Enable the module via Syringe's plugin loading mechanism (depends on Syringe configuration)
4. Launch the game

---

## Building

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

## Controls

| Action | Effect |
|--------|--------|
| **Ctrl + scroll wheel up** | Zoom in (magnify) |
| **Ctrl + scroll wheel down** | Zoom out (reduce) |
| **Double press Ctrl** (within 400 ms) | Reset the zoom to 1.0x |
| **Arrow keys** (while zoomed) | Pan the magnified view |

All zoom parameters are compile-time constants (`src\Zoomer.hpp`). To adjust them, modify the source code and recompile.

---

## Compatibility

This plugin hooks the game's own render and input flow through Syringe and does not rewrite the executable on disk. It is theoretically compatible with:

- Original Yuri's Revenge 1.001
- Other Syringe-based modded clients

If you encounter compatibility issues with a specific mod, please open an Issue with details.

---

## Technical Implementation

- **Injection framework**: Syringe (`DEFINE_HOOK` code hooks; Ares startup injection, handles DLL loading and initialization)
- **Input**: subclasses the game window procedure (`SetWindowLongPtrW`) for the wheel and hotkey handling; the cursor position itself is never remapped
- **Rendering**: hooks the frame pre/post render points, backs up the view rows and upscales the source rect (nearest neighbour) over the drawn frame; viewport width/height reads are clamped to the magnified source rect
- **Camera**: moves the game camera on every zoom step so the focused point stays under the cursor; undo on reset
- **Coordinate transforms**: tactical click, selection rubber band start/end and right drag speed are un-magnified at the exact game locations that convert screen space to game space (same four spots Telescope uses)

---

## Credits

- Project creator & core author: ChoyuTsumu
- Development assistant: Sovietianqi

Contributions and pull requests are welcome.

---

## License

This project is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE) for details.

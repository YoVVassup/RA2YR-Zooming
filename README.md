# ViewCtrl — Free Zoom Plugin for C&C: Yuri's Revenge

---

## Overview

A viewport zoom enhancement plugin for Command & Conquer: Yuri's Revenge. It uses runtime hooking to dynamically intervene in the game's rendering viewport parameters, providing smooth, customizable zoom capability.

Built on **MinHook** and the **Windows API**, with **Syringe** (the injection framework used by Ares) for injection and lifecycle management. Stable and compatible.

---

## Features

- Smooth viewport zoom via mouse wheel
- Adjustable zoom range and step precision
- Does not modify the game executable or resources
- Non-intrusive to the rendering pipeline, minimal performance overhead

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

## Controls

| Action | Effect |
|--------|--------|
| Scroll wheel up | Zoom in (magnify) |
| Scroll wheel down | Zoom out (reduce) |

All zoom parameters are compile-time constants. To adjust them, modify the source code and recompile.

---

## Compatibility

This plugin hooks low-level APIs and does not depend on specific game logic implementations. It is theoretically compatible with:

- Original Yuri's Revenge 1.001
- Other Syringe-based modded clients

If you encounter compatibility issues with a specific mod, please open an Issue with details.

---

## Technical Implementation

- **Hook library**: MinHook (stable x86 inline hooking)
- **System interfaces**: Windows API (`GetCursorPos`, `SetWindowLongPtr`, etc. for input capture and window message processing)
- **Injection framework**: Syringe (Ares startup injection, handles DLL loading and initialization)
- **Rendering intervention**: Hooks DirectDraw Blt and the window procedure to inject zoom parameters in real time

---

## Credits

- Project creator & core author: ChoyuTsumu
- Development assistant: Sovietianqi

Contributions and pull requests are welcome.

---

## License

This project is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE) for details.

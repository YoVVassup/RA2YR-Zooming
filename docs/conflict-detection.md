# Scaler Conflict Detection

ViewCtrl magnifies the tactical view at the render level. If a second
scaler is also active, the frame is transformed twice and the coordinate
un-mapping fights itself. `ScalerConflict` detects competitors and keeps
ViewCtrl inert when one is found.

All layers live in `src/ScalerConflict.{hpp,cpp}`; the reactive watch is
fed from `RenderZoom::PreRender`. `GameInt` (`0x52CAE9`) calls
`ScalerConflict::Present()` before `Zoomer::Init()`; when it returns true
the plugin does nothing — no init thread, no window subclass, every hook
replays the original instructions.

---

## Layers

### L1 — module file name

`IsScalerModuleName` matches a hard-coded list of known scaler file names
(case-insensitive):

```
gscript.ext
telescope.dll
moexperience.dll
kaleidoscope.dll
ra2vampirezoom.loader.dll
```

Cheap, runs first. Defeated by a rename — hence the layers below.

### L2 — `.syhks00` hook-table fingerprint

`ImageClaimsScalerShare` walks a foreign PE image's `.syhks00` section
(Syringe's declarative hook table) and counts how many of ViewCtrl's eight
*fingerprint* sites it claims. The fingerprint is all nine hook sites
except `0x52CAE9`, which Ares legitimately shares with everyone.

- `Present()` asks for **≥ 1** claim: a plugin that owns any non-Ares site
  of this pipeline re-implements the same zoom.
- Offline / research threshold is **≥ 3**: Telescope claims all nine,
  Kaleidoscope six; ordinary framework plugins share at most Ares's site.

Hook declarations are `align(16)` records of
`{hookAddr: u32, hookSize: u32, namePtr: u32, pad: u32}`.

### L3 — runtime byte sniff

`SiteHookedForeign` reads five bytes at each fingerprint site and decodes a
rel32 `E9` jump (`DecodeJumpTarget`). The jump target is resolved with
`GetModuleHandleExA(FROM_ADDRESS)`; if it lands in a loaded module that is
not the host executable, gamemd.exe or ViewCtrl itself, a foreign direct
patcher has overwritten the site.

This is the MOExperience pattern: it rewrites `0x4F451B` from `DllMain` and
again when ScaleBar is toggled, clobbering Syringe's dispatcher stub.

`RuntimePatchPresent()` runs:

- once at `Present()` (GameInt), and
- throttled once per second from `RenderZoom::PreRender` while magnified
  (`PATCH_SCAN_MS`), because the re-patch can happen mid-game.

A jump into unallocated memory (Syringe's own stubs may live there) is
**not** blamed on anyone — conservative on purpose.

### L4 — reactive view-size watch

`ObserveViewDims(windowW, windowH, viewW, viewH, nowMs)` is called from
`RenderZoom::PreRender` with the vanilla pair (never the substituted source
rect, so ViewCtrl never trips over its own scaling).

- A window-size change re-baselines the watch (resize, video-mode switch,
  Vampire-style display scaler).
- A view-size change while the window holds still starts a pending timer.
- If the displacement outlives `REACTIVE_PERSIST_MS` (1000 ms), it is a
  world-space scaler rescaling the view under us — sticky conflict.

`ReactiveConflict()` is sticky; `ResetReactive()` clears it (tests /
shutdown). `RenderZoom::PreRender` calls `Disable("a world-space scaler is
rescaling the view")` when it trips, which unsubstitutes the view bounds,
unmagnifies and shuts the Zoomer down for the rest of the session.

### L5 — known exports

`ModuleExportsScaler` probes a loaded module with `GetProcAddress` for
scaler-specific entry points:

```
_exfunc_get_zoom_factor@0   (GScript, stdcall-decorated)
exfunc_get_zoom_factor      (undecorated form)
```

A renamed GScript build (beating L1) still exposes its zoom factor to its
own script engine; this catches it. Runs inside the same module walk as L1
and L2, so it costs one `GetProcAddress` per candidate name per foreign
module, once at GameInt.

---

## Disable path

When any layer fires — at GameInt or later from PreRender — the module is
disabled for the rest of the session:

1. `RenderZoom::Disable(reason)` — restores substituted view bounds if any,
   drops the scratch buffer, logs the reason, clears `g_enabled`.
2. `Zoomer::Shutdown()` — restores the window procedure, disables the game
   camera hook, joins the init thread.
3. Every hook continues to replay the original instructions.

There is no re-enable path: a scaler that appeared once will not vanish
reliably, and flapping between modes would be worse than staying off.

---

## What is *not* a conflict

- **Ares / Phobos / Kratos** — share `0x52CAE9` or nearby DrawOnTop sites,
  claim none of the eight fingerprint sites, export no zoom API.
- **cnc-ddraw / DDrawCompat / ts-ddraw** — display-layer scalers; they
  change the window size, which L4 re-baselines. Vampire.Zoom is in the L1
  list because its zoom is intentional and compositional, not because the
  technique is unsafe.
- **The game itself resizing** — window resize and video-mode switches
  move both window and view; L4 treats that as a new baseline.

---

## Test coverage

`tests/test_zoomer.cpp` covers (69 cases / 653 assertions, **99.7 %** of
`src/` under OpenCppCoverage; only three defensive OS-failure branches
remain unexercised):

- L1 name matching (positive and negative cases, case-insensitivity).
- L2 `ImageClaimsScalerShare` against synthetic `.syhks00` images and the
  real nine-site table of this plugin, including the `wrongSection` /
  `noSection` / bad-header edge cases.
- L3 `DecodeJumpTarget` (JMP / non-JMP / bounds), `SiteHookedForeign`
  with a fake foreign module, and the full `RuntimePatchPresent` /
  `PreRender` L3-disable path (planted E9 at a fingerprint site that
  lands in `l5_export_stub.dll`).
- L4 the full reactive timeline: baseline, transient, persistent,
  re-baseline on resize.
- L5 `IsScalerExportName` (positive and negative), `ModuleExportsScaler`
  against kernel32 / the test exe / null / a real renamed module
  (`l5_export_stub.dll`, built as a pre-build step), and the
  `Present()` walk that fires the L5 probe.

Test-only seams (`VIEWCTRL_TEST`) used to reach defensive branches:

| Seam | Purpose |
|------|---------|
| `GameCamera::SetCameraProbeAddressForTest` | point `Enable()` at a controllable prologue |
| `Debug::SetLogEnabledForTest` / `LogForceDisabled` | force the log on/off regardless of env |
| `ScalerConflict::SetFailOwnImageForTest` | make `OwnImage()` return null |
| `RenderZoom::SetLastPatchScanForTest` | force the L3 patch scan on the next frame |

The SEH guards around the hook bodies (`src/Seh.h`) are compiled out under
`VIEWCTRL_TEST` so a deliberate edge case in the tests crashes the test
executable instead of being silently swallowed.

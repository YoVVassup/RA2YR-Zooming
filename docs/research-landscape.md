# Zoom Landscape for C&C: Yuri's Revenge

A survey of every tactical-view zoom implementation found in the wild while
designing ViewCtrl's conflict detector. The goal is engineering, not ranking:
each entry records *how* the zoom is done, which hook sites it claims, and
what that means for co-loading with ViewCtrl.

Research date: 2026. Sources: public binaries (ModDB, GitHub releases),
PE/COFF disassembly, `.syhks00` hook tables, export tables, and the
[YR Hook Encyclopedia](https://github.com/Phobos-developers/YRpp).

---

## Summary table

| Implementation | Technique | Zoom range | Hook sites claimed (of ViewCtrl's 17) | Detect layer |
|---|---|---|---|---|
| **ViewCtrl** (this repo) | World-space: substitute `ViewBounds` with a source rect, nearest-neighbour stretch after the game draws | 1.0–4.0, gear 1.15 | all 17 (own) | — |
| **Telescope v1.4** | Fork of an earlier ViewCtrl: same nine sites, same un-magnify transforms | 1.0–4.0 | **9 of 17** | L1 name, L2 table |
| **Kaleidoscope v1.1** | World-space rescale, independent reimplementation | 0.5–2.0 | **6 of 17** | L1 name, L2 table |
| **GScript.ext** (Tiberium Crisis 2) | Render-scale factor applied inside `GScreenClass::DrawOnTop`, exposed to its script engine | 1.0–2.0, step 0.2 | none of 17 (hooks sidebar wheel at `0x5371B0`/`0x537240`) | L1 name, L4 reactive, L5 export |
| **MOExperience** (Mental Omega) | Hybrid: a `ScaleBar` UI (9 steps, 0.25×–MAX) plus a runtime `E9` patch of `0x4F451B` | 0.25×–configured max | **re-patches `0x4F451B` at runtime** | L1 name, L3 byte sniff |
| **Ra2.Vampire.Zoom** | Video-mode switch through a forked cnc-ddraw (`DDEnableZoom`, Mr. Stalina's) | experimental | none (display layer) | L1 name |

Display-layer wrappers (cnc-ddraw, DDrawCompat, ts-ddraw) and CnCNet's
launcher do **not** implement a tactical zoom; they only scale the final
DirectDraw present. They coexist with ViewCtrl because ViewCtrl magnifies
inside `DSurface::Composite` before `Frame::Present`.

---

## Per-implementation notes

### Telescope v1.4

A direct fork of the pre-world-space ViewCtrl hook set. Its `.syhks00`
declares the same nine addresses (`0x52CAE9`, `0x4F44AF`, `0x4F451B`,
`0x6D864E`, `0x6D868A`, `0x692325`, `0x6D9F80`, `0x6D9FC0`, `0x693791`).
Co-loading with ViewCtrl would double-hook every site; Syringe would chain
the two handlers and the frame would be transformed twice. Caught by L1
(file name) and L2 (`.syhks00` claims all nine).

### Kaleidoscope v1.1

Independent world-space implementation, range 0.5×–2.0×. Claims six
fingerprint sites in its own `.syhks00`. Same double-transform hazard
as Telescope. Caught by L1 and L2.

### GScript.ext

Not a standalone zoom plugin: the engine framework of the Tiberium Crisis 2
total conversion (Axion Gear). ~800 hooks, 740 exports. Zoom is one option:

- Factor: `float` at VA `0x101739B0`, clamped to [1.0, 2.0], step 0.2.
- Two hooks: `SidebarUpCommandClass_WheelScrollUp_Zoom` at gamemd `0x5371B0`
  and `SidebarDownCommandClass_WheelScrollDown_Zoom` at `0x537240` (size 9).
- Consumed inside `GScreenClass::DrawOnTop_UpadteLater` as a render-scale
  factor; exported to GScript's script engine as `_exfunc_get_zoom_factor@0`.
- Sources: `github.com/freez/GScript` (from the PDB path) — repository is
  deleted or private; no mirrors found.

None of ViewCtrl's seventeen sites is claimed, so L2 never fires. A renamed
GScript build is still caught by L4 (the view size moves while the window
holds still) and L5 (the zoom export).

### MOExperience (Mental Omega)

The only known *runtime* patcher of ViewCtrl's sites. From its `DllMain` —
and again whenever the in-game ScaleBar is toggled — it writes an `E9`
rel32 jump over `0x4F451B` (post-render), redirecting to its own trampoline.
Syringe-installed hooks are overwritten in place. ViewCtrl's L3 byte sniff
re-scans the sixteen fingerprint sites once a second while magnified and once
at `GameInt`; a foreign JMP target disables the module immediately.

The ScaleBar itself is a 9-step discrete scale control (0.25× up to a
configured maximum), not a continuous wheel zoom.

### Ra2.Vampire.Zoom

Does not touch gamemd's render path. It swaps in a forked cnc-ddraw that
exposes a `DDEnableZoom` export and switches the video mode to simulate
magnification. From ViewCtrl's perspective this is a display-layer scaler:
the window client size changes, which re-baselines the L4 reactive watch
and is never flagged. The two can co-load; the visual result is a
composition of both magnifications, which is usually not what the player
wants, so the file name is still in the L1 list.

---

## Implications for ViewCtrl

1. **Static layers (L1, L2, L5) run once at `GameInt`.** They catch every
   known scaler that is loaded before the game finishes initialising —
   which is all of them, because Syringe loads plugins before `InitGame`.
2. **L3 is the only defence against a mid-game re-patch** (MOExperience's
   ScaleBar toggle). It has to re-scan while the game runs.
3. **L4 is the catch-all** for a future world-space scaler that renames
   itself, ships no useful exports and claims none of our sites: the view
   dimensions still move while the window does not.
4. **Display-layer zoomers (Vampire, ddraw forks) must not be treated as
   hard conflicts** — they change the window size, not the view rect, and
   the L4 watch re-baselines correctly.

See [conflict-detection.md](conflict-detection.md) for how the layers are
implemented in `ScalerConflict` / `RenderZoom`.

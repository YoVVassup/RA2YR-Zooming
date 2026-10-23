#pragma once

#include <windows.h>

// Detection of competing view scalers.
//
// Big mods ship their own zoom: GScript.ext scales the view for the
// Tiberium Crisis mod, Telescope v1.4 bundles a fork of this zoom
// pipeline, Kaleidoscope and MOExperience rescale the tactical view
// themselves, and Ra2.Vampire.Zoom switches the video mode through a
// cnc-ddraw fork. Two scalers transforming the same frame would
// double-magnify and fight over the coordinate un-mapping, so GameInt
// calls Present() before Zoomer::Init() and leaves this plugin inert when
// a competitor is loaded: no init thread, no window subclass, and every
// hook keeps replaying the original instructions.
//
// Detection is layered so no known scaling mechanism slips through:
//
//   L1  module file name - the five known scaler file names.
//   L2  .syhks00 table   - any foreign module claiming one of our
//                          scaler-fingerprint hook sites (render, clamp,
//                          coordinate paths — Kaleidoscope claims six,
//                          Telescope the whole older set) re-implements
//                          this pipeline. The Ares init site is
//                          framework-shared and never counts.
//   L3  runtime bytes    - a foreign module that JMP-patched one of those
//                          sites after Syringe installed our hooks
//                          (MOExperience rewrites 0x4F451B from its DllMain
//                          and again when ScaleBar is toggled).
//   L4  reactive watch   - ViewBounds.W/H moving while the window size holds
//                          still is a world-space scaler rescaling the view
//                          under us (GScript, Kaleidoscope, MOExperience;
//                          caught mid-game even under a renamed file).
//   L5  known exports    - a foreign module exporting a scaler-specific
//                          entry point (GScript exposes its zoom factor to
//                          its script engine as _exfunc_get_zoom_factor@0);
//                          survives a rename that would fool L1.
namespace ScalerConflict {

// True when `fileName` (a module file name such as L"GScript.ext") belongs
// to a known competing scaler. Case-insensitive.
bool IsScalerModuleName(const wchar_t* fileName);

// True when `name` (an undecorated or stdcall-decorated export name) is a
// scaler-specific entry point. Case-sensitive, as PE exports are.
bool IsScalerExportName(const char* name);

// True when the module `mod` exports any name accepted by
// IsScalerExportName. GetProcAddress-based; a null module is false.
bool ModuleExportsScaler(HMODULE mod);

// True when the PE image at `image` declares at least `minClaimed` of
// [sites, sites + count) in its .syhks00 section. A plugin that re-implements
// that much of our hook set is another scaler (Telescope claims the whole
// set); ordinary framework plugins share at most the framework-shared
// site (the Ares init hook).
bool ImageClaimsScalerShare(const unsigned char* image,
	const unsigned int* sites, unsigned int count,
	unsigned int minClaimed = 3);

// Collects the hook addresses declared in this module's own .syhks00.
// Returns how many were stored, capped at `cap`.
unsigned int OwnHookSites(unsigned int* out, unsigned int cap);

// --- L3: runtime byte sniff -------------------------------------------------

// Decodes a rel32 JMP at `at` (site address `site`) into its absolute
// target. False when the bytes are not a JMP.
bool DecodeJumpTarget(const unsigned char* at, uintptr_t site,
	uintptr_t& target);

// True when the five bytes at `siteAddr` start a rel32 JMP whose target
// lives inside a loaded module that is neither this plugin, the host
// executable nor gamemd.exe - i.e. a foreign direct patcher (MOExperience
// style BuildTrampAt), not a Syringe dispatcher stub. Unreadable sites and
// jumps into unallocated memory are not foreign.
bool SiteHookedForeign(uintptr_t siteAddr);

// True when any of our own hook sites is currently patched by a foreign
// module. Present() folds this in; RenderZoom re-checks it throttled while
// the game runs.
bool RuntimePatchPresent();

// --- L4: reactive view-size watch ------------------------------------------

// Observes the tactical view size the game reports. A view dimension that
// changes while the window dimensions hold still - persistently, past
// REACTIVE_PERSIST_MS - is a foreign world-space scaler; a window dimension
// change is a resize or video mode switch and re-baselines the watch.
void ObserveViewDims(int windowW, int windowH, int viewW, int viewH,
	DWORD nowMs);

// Sticky: once true, only ResetReactive() clears it.
bool ReactiveConflict();

void ResetReactive();

// True when another loaded module is a known scaler, exports a
// scaler-specific entry point, claims one of our hook sites, or has already
// patched one of them at runtime. The host executable and this module itself
// are never conflicts.
bool Present();

#ifdef VIEWCTRL_TEST
// Force-fail seams so the OwnImage and EnumProcessModules OS-failure
// branches are reachable under unit tests.
void SetFailOwnImageForTest(bool fail);
void SetFailEnumModulesForTest(bool fail);
#endif

} // namespace ScalerConflict

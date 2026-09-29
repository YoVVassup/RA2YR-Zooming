#pragma once

// Detection of competing view scalers.
//
// Big mods ship their own zoom: GScript.ext scales the view for the
// Tiberium Crisis mod, Telescope v1.4 bundles a fork of these very nine
// hooks. Two scalers transforming the same frame would double-magnify and
// fight over the coordinate un-mapping, so GameInt calls Present() before
// Zoomer::Init() and leaves this plugin inert when a competitor is loaded:
// no init thread, no window subclass, and every hook keeps replaying the
// original instructions.
namespace ScalerConflict {

// True when `fileName` (a module file name such as L"GScript.ext") belongs
// to a known competing scaler. Case-insensitive.
bool IsScalerModuleName(const wchar_t* fileName);

// True when the PE image at `image` declares at least three of
// [sites, sites + count) in its .syhks00 section. A plugin that re-implements
// that much of our hook set is another scaler (Telescope claims all nine);
// ordinary framework plugins share at most a single site with us.
bool ImageClaimsScalerShare(const unsigned char* image,
	const unsigned int* sites, unsigned int count);

// Collects the hook addresses declared in this module's own .syhks00.
// Returns how many were stored, capped at `cap`.
unsigned int OwnHookSites(unsigned int* out, unsigned int cap);

// True when another loaded module is a known scaler or claims enough of
// our hook sites to be one. The host executable and this module itself
// are never conflicts.
bool Present();

} // namespace ScalerConflict

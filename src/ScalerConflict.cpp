#include "ScalerConflict.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstring>

#pragma comment(lib, "psapi.lib")

namespace {

	// Modules that scale the view on their own; ViewCtrl must not compete.
	// GScript.ext ships with the Tiberium Crisis mod, Telescope v1.4 is a
	// fork of this plugin, Kaleidoscope v1.1 and MOExperience.dll rescale
	// the tactical view, Ra2.Vampire.Zoom switches the video mode through a
	// cnc-ddraw fork.
	const wchar_t* const kScalerModules[] = {
		L"gscript.ext",
		L"telescope.dll",
		L"moexperience.dll",
		L"kaleidoscope.dll",
		L"ra2vampirezoom.loader.dll",
	};

	// L5: scaler-specific export names. A renamed module (beating L1) that
	// still exposes its zoom API to its own scripts is caught here. GScript
	// declares _exfunc_get_zoom_factor@0 (stdcall, ordinal-free name table)
	// next to the rest of its _exfunc_* script API; the undecorated form is
	// listed too in case a future build exports it plain.
	const char* const kScalerExports[] = {
		"_exfunc_get_zoom_factor@0",
		"exfunc_get_zoom_factor",
	};

	// A single shared hook site is normal between YR framework plugins only
	// on 0x52CAE9 (Ares is documented to sit there); any of the other eight
	// sites is exclusive to zoom pipelines, so Present() asks for one.
	constexpr unsigned int kMinClaimedSites = 3;
	constexpr unsigned int kPresentMinClaimedSites = 1;

	// GameInt (0x52CAE9) is deliberately absent: Ares shares it, every
	// framework plugin may. The remaining eight sites are the scaler
	// fingerprint.
	constexpr unsigned int kAresSite = 0x52CAE9u;

	// hookdecl is align(16)/pack(16): {hookAddr, hookSize, name ptr, pad}.
	constexpr unsigned int kHookDeclSize = 16;

	constexpr unsigned int kMaxOurSites = 16;

	// A view dimension that stays displaced this long while the window size
	// holds still is a foreign scaler, not a transient engine update.
	constexpr DWORD REACTIVE_PERSIST_MS = 1000;

	// Walks the .syhks00 section of the PE image at `image`, invoking
	// fn(hookAddr) per declaration; stops early when fn returns true.
	template <typename F>
	bool ForEachHookDecl(const unsigned char* image, F&& fn)
	{
		if (!image)
			return false;

		const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		if (dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) || dos->e_lfanew > 0x1000)
			return false;

		const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;

		const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
		{
			if (std::memcmp(sec[i].Name, ".syhks00", 8) != 0)
				continue;

			const unsigned char* table = image + sec[i].VirtualAddress;
			const unsigned int bytes = sec[i].Misc.VirtualSize;
			for (unsigned int off = 0; off + sizeof(unsigned int) <= bytes; off += kHookDeclSize)
			{
				unsigned int addr = 0;
				std::memcpy(&addr, table + off, sizeof(addr));
				if (addr == 0)
					break; // zero padding past the real declarations
				if (fn(addr))
					return true;
			}
			return false;
		}
		return false;
	}

	// Resolves the image base of the module this code is linked into
	// (ViewCtrl.dll in the game, the test executable under the unit tests).
#ifdef VIEWCTRL_TEST
	// Force-fail seam for GetModuleHandleEx on our own image.
	bool g_failOwnImage = false;
#endif

	const unsigned char* OwnImage()
	{
#ifdef VIEWCTRL_TEST
		if (g_failOwnImage)
			return nullptr;
#endif
		HMODULE self = nullptr;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			reinterpret_cast<LPCSTR>(&OwnImage), &self))
			return nullptr;
		return reinterpret_cast<const unsigned char*>(self);
	}

	bool IsReadableLocal(const void* address, size_t bytes)
	{
		if (!address || bytes == 0) return false;

		MEMORY_BASIC_INFORMATION mbi = {};
		if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
		if (mbi.State != MEM_COMMIT) return false;
		if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;

		const auto* start = static_cast<const BYTE*>(address);
		const auto* regionEnd = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
		return start + bytes <= regionEnd;
	}

	// The eight scaler-fingerprint sites: OwnHookSites() minus 0x52CAE9.
	unsigned int FingerprintSites(unsigned int* out, unsigned int cap)
	{
		unsigned int all[kMaxOurSites] = {};
		const unsigned int n = ScalerConflict::OwnHookSites(all, kMaxOurSites);
		unsigned int kept = 0;
		for (unsigned int i = 0; i < n && kept < cap; ++i)
		{
			if (all[i] == kAresSite)
				continue;
			out[kept++] = all[i];
		}
		return kept;
	}

	// L4 watch state: baseline plus an optional pending displacement.
	struct ReactiveWatch
	{
		bool have = false;
		bool pending = false;
		bool conflict = false;
		int windowW = 0;
		int windowH = 0;
		int viewW = 0;
		int viewH = 0;
		DWORD pendingSince = 0;
	};

	ReactiveWatch g_reactive;

} // namespace

namespace ScalerConflict {

bool IsScalerModuleName(const wchar_t* fileName)
{
	if (!fileName || !*fileName)
		return false;

	for (const wchar_t* name : kScalerModules)
	{
		if (_wcsicmp(fileName, name) == 0)
			return true;
	}
	return false;
}

bool IsScalerExportName(const char* name)
{
	if (!name || !*name)
		return false;

	for (const char* known : kScalerExports)
	{
		if (std::strcmp(name, known) == 0)
			return true;
	}
	return false;
}

bool ModuleExportsScaler(HMODULE mod)
{
	if (!mod)
		return false;

	for (const char* name : kScalerExports)
	{
		if (GetProcAddress(mod, name))
			return true;
	}
	return false;
}

bool ImageClaimsScalerShare(const unsigned char* image,
	const unsigned int* sites, unsigned int count,
	unsigned int minClaimed)
{
	if (!sites || !count || minClaimed == 0)
		return false;
	if (minClaimed > count)
		return false;

	unsigned int claimed = 0;
	ForEachHookDecl(image, [&](unsigned int addr) {
		for (unsigned int i = 0; i < count; ++i)
		{
			if (sites[i] == addr)
			{
				++claimed;
				break;
			}
		}
		return claimed >= minClaimed;
	});
	return claimed >= minClaimed;
}

unsigned int OwnHookSites(unsigned int* out, unsigned int cap)
{
	if (!out || !cap)
		return 0;

	const unsigned char* self = OwnImage();
	if (!self)
		return 0;

	unsigned int n = 0;
	ForEachHookDecl(self, [&](unsigned int addr) {
		if (n < cap)
			out[n] = addr;
		++n;
		return false;
	});
	return n < cap ? n : cap;
}

bool DecodeJumpTarget(const unsigned char* at, uintptr_t site,
	uintptr_t& target)
{
	if (!at || at[0] != 0xE9)
		return false;

	int rel32 = 0;
	std::memcpy(&rel32, at + 1, sizeof(rel32));
	target = site + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel32));
	return true;
}

bool SiteHookedForeign(uintptr_t siteAddr)
{
	const auto* at = reinterpret_cast<const unsigned char*>(siteAddr);
	if (!IsReadableLocal(at, 5))
		return false;

	uintptr_t target = 0;
	if (!DecodeJumpTarget(at, siteAddr, target))
		return false;

	// The JMP has to resolve into a loaded module before it can be blamed
	// on anyone: Syringe's own dispatcher stubs may live in unallocated
	// memory, and a jump there is our hook, not a foreign one.
	HMODULE targetMod = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
		reinterpret_cast<LPCSTR>(target), &targetMod))
		return false;

	const HMODULE exe = GetModuleHandleW(nullptr);
	const HMODULE self = reinterpret_cast<HMODULE>(
		const_cast<unsigned char*>(OwnImage()));
	const HMODULE game = GetModuleHandleA("gamemd.exe");
	const bool known = (targetMod == exe || targetMod == self || targetMod == game);
	FreeLibrary(targetMod); // GetModuleHandleEx took a reference
	return !known;
}

bool RuntimePatchPresent()
{
	unsigned int sites[16] = {};
	const unsigned int n = FingerprintSites(sites, 16);
	for (unsigned int i = 0; i < n; ++i)
	{
		if (SiteHookedForeign(sites[i]))
			return true;
	}
	return false;
}

void ObserveViewDims(int windowW, int windowH, int viewW, int viewH,
	DWORD nowMs)
{
	ReactiveWatch& r = g_reactive;
	if (r.conflict)
		return;

	// First look, or the window itself changed size (user resize, video
	// mode switch, Vampire.Zoom style scaling): everything is legitimate
	// again and the new sizes become the baseline.
	if (!r.have || windowW != r.windowW || windowH != r.windowH)
	{
		r.have = true;
		r.pending = false;
		r.windowW = windowW;
		r.windowH = windowH;
		r.viewW = viewW;
		r.viewH = viewH;
		return;
	}

	if (viewW == r.viewW && viewH == r.viewH)
	{
		r.pending = false;
		return;
	}

	// The window held still while the view area moved - a world-space
	// scaler rescaling the tactical view under us. Transient engine
	// updates clear themselves; a displacement that outlives
	// REACTIVE_PERSIST_MS is the real thing.
	if (!r.pending)
	{
		r.pending = true;
		r.pendingSince = nowMs;
		return;
	}
	if (nowMs - r.pendingSince >= REACTIVE_PERSIST_MS)
		r.conflict = true;
}

bool ReactiveConflict()
{
	return g_reactive.conflict;
}

void ResetReactive()
{
	g_reactive = ReactiveWatch{};
}

#ifdef VIEWCTRL_TEST
void SetFailOwnImageForTest(bool fail)
{
	g_failOwnImage = fail;
}
#endif

bool Present()
{
	unsigned int sites[kMaxOurSites] = {};
	const unsigned int siteCount = FingerprintSites(sites, kMaxOurSites);

	const HMODULE exe = GetModuleHandleW(nullptr);
	const unsigned char* self = OwnImage();

	// EnumProcessModules sees every mapped image (including test doubles
	// loaded without a full LoadLibrary path); Toolhelp can miss those.
	HMODULE mods[256] = {};
	DWORD needed = 0;
	if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
		return false;
	const unsigned int count = (needed > sizeof(mods))
		? (unsigned)(sizeof(mods) / sizeof(mods[0]))
		: (unsigned)(needed / sizeof(HMODULE));

	bool found = false;
	for (unsigned int i = 0; i < count; ++i)
	{
		const HMODULE mod = mods[i];
		if (!mod || mod == exe || reinterpret_cast<const unsigned char*>(mod) == self)
			continue;

		wchar_t fileName[MAX_PATH] = {};
		GetModuleFileNameW(mod, fileName, MAX_PATH);
		const wchar_t* baseName = fileName;
		if (const wchar_t* slash = wcsrchr(fileName, L'\\'))
			baseName = slash + 1;

		// L1: known scaler file name.
		if (IsScalerModuleName(baseName))
		{
			found = true;
			break;
		}
		// L5: scaler-specific export, survives a rename.
		if (ModuleExportsScaler(mod))
		{
			found = true;
			break;
		}
		// L2: any of the eight fingerprint sites in the foreign
		// .syhks00 table.
		if (siteCount
			&& ImageClaimsScalerShare(reinterpret_cast<const unsigned char*>(mod),
				sites, siteCount, kPresentMinClaimedSites))
		{
			found = true;
			break;
		}
	}
	if (found)
		return true;

	// L3: a foreign module that already patched one of the sites.
	return RuntimePatchPresent();
}

} // namespace ScalerConflict

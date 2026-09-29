#include "ScalerConflict.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <cstring>

namespace {

	// Modules that scale the view on their own; ViewCtrl must not compete.
	const wchar_t* const kScalerModules[] = {
		L"gscript.ext",
		L"telescope.dll",
	};

	// A single shared hook site is normal between YR framework plugins
	// (Ares is documented to sit on 0x52CAE9); re-implementing a third of
	// our set means the module is another copy of the zoom pipeline.
	constexpr unsigned int kMinClaimedSites = 3;

	// hookdecl is align(16)/pack(16): {hookAddr, hookSize, name ptr, pad}.
	constexpr unsigned int kHookDeclSize = 16;

	constexpr unsigned int kMaxOurSites = 16;

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
	const unsigned char* OwnImage()
	{
		HMODULE self = nullptr;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			reinterpret_cast<LPCSTR>(&OwnImage), &self))
			return nullptr;
		return reinterpret_cast<const unsigned char*>(self);
	}

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

bool ImageClaimsScalerShare(const unsigned char* image,
	const unsigned int* sites, unsigned int count)
{
	if (!sites || !count)
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
		return claimed >= kMinClaimedSites;
	});
	return claimed >= kMinClaimedSites;
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

bool Present()
{
	unsigned int sites[kMaxOurSites] = {};
	const unsigned int siteCount = OwnHookSites(sites, kMaxOurSites);

	const HMODULE exe = GetModuleHandleW(nullptr);
	const unsigned char* self = OwnImage();

	const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snap == INVALID_HANDLE_VALUE)
		return false;

	bool found = false;
	MODULEENTRY32W me{};
	me.dwSize = sizeof(me);
	if (Module32FirstW(snap, &me))
	{
		do
		{
			const HMODULE mod = reinterpret_cast<HMODULE>(me.hModule);
			if (mod == exe || reinterpret_cast<const unsigned char*>(mod) == self)
				continue;

			if (IsScalerModuleName(me.szModule))
			{
				found = true;
				break;
			}
			if (siteCount
				&& ImageClaimsScalerShare(reinterpret_cast<const unsigned char*>(mod), sites, siteCount))
			{
				found = true;
				break;
			}
		} while (Module32NextW(snap, &me));
	}
	CloseHandle(snap);
	return found;
}

} // namespace ScalerConflict

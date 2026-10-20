#pragma once

#include "Log.h"
#include <windows.h>

// SEH guard for the Syringe hook bodies.
//
// Every hook runs inside the game process with whatever else is loaded next
// to us: a foreign plugin that already corrupted memory, an unreadable game
// global on an exotic install, a surface that vanished mid-frame. An
// unhandled access violation there takes the whole game down with it. The
// guard turns such a fault into a logged no-op: the hook finishes with the
// original instructions replayed and the game keeps running degraded instead
// of crashing.
//
// Disabled under VIEWCTRL_TEST on purpose: the unit tests call the same
// bodies with deliberate edge cases, and a swallowed fault there would hide
// a real regression behind a mysterious "nothing happened".
namespace ViewCtrlSeh {

inline LONG Swallow(PEXCEPTION_POINTERS ep)
{
	const EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : nullptr;
	LOG("SEH exception 0x%08lX at %p — hook continues with the original instructions",
		rec ? rec->ExceptionCode : 0u,
		rec ? rec->ExceptionAddress : nullptr);
	return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace ViewCtrlSeh

#ifdef VIEWCTRL_TEST
#define VIEWCTRL_SEH_BEGIN {
#define VIEWCTRL_SEH_END }
#else
#define VIEWCTRL_SEH_BEGIN __try {
#define VIEWCTRL_SEH_END } __except (ViewCtrlSeh::Swallow(GetExceptionInformation())) {}
#endif

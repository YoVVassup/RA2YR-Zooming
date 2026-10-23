#pragma once

#include <windows.h>

// Intercept + replay of HUD draws while the tactical view is magnified.
//
// While the view is substituted the game renders only the source rect and
// RenderZoom stretches that small frame over the full view afterwards.
// Everything drawn between the two render hooks lands inside the source
// rect and comes out magnified by the stretch — MO's battle overlay timer
// drew ~1.5x (13-14px against 9px native), its backing blit came out as a
// black stretched band.
//
// The Kaleidoscope reference (v1.1, the same trick in production) hooks
// three sites instead of the text funnels:
//
//   TacticalClass::DrawTimer          0x6D4B50  (skip to `ret 0x10`)
//   TacticalClass::DrawScreenText     0x6D4E20  (skip to `ret 4`)
//   GScreenClass::Render DrawUI vcall 0x4F44F9  (skip the call)
//
// Each is captured while the frame is armed; after RenderZoom stretched
// the frame AND put the vanilla view dimensions back, ReplayDeferred()
// re-invokes the originals. The originals recompute their layout from the
// restored dimensions — timers and screen text position themselves, DrawUI
// redraws the whole UI pass — so the glyphs and backings land at native
// size on top of the stretch, before the cursor blit and Present.
//
// No position mapping and no region gate: the layout comes from the game
// itself on the replay. The xy pointers and the DrawScreenText/DrawUI
// `this` + arg are forwarded exactly as recorded (stable object members
// for the frame the record lives in); the timer string is copied, a caller
// may hand over a scratch buffer. Outside the game the records are
// dropped: the addresses only point at real code inside gamemd.exe.
class TextDefer
{
public:
	enum class Kind
	{
		Timer,       // TacticalClass::DrawTimer
		ScreenText,  // TacticalClass::DrawScreenText
		UI           // GScreenClass::Render DrawUI virtual call
	};

	// Arms the collector for this frame: src dimensions > 0 mean the frame
	// is substituted. Clears whatever a skipped PostRender left behind.
	static void BeginFrame(const RECT& view, int srcW, int srcH);

	// Drops every record and disarms the collector (PostRender safety net):
	// no record ever outlives the frame it was captured in.
	static void Reset();

	// How many calls are waiting for the replay.
	static int RecordCount();

	// Hook side. Arguments mirror the call in question:
	//   Timer       — index (ECX), scheme (EDX), Time/Text/xy1/xy2 (stack).
	//   ScreenText  — this (ECX), a Point2D* member (stack).
	//   UI          — this (ESI), the virtual call's dword arg (stack).
	// True = captured, the hook must skip to the given tail.
	static bool TryDeferTimer(int index, void* scheme, int time,
		const wchar_t* text, void* xy1, void* xy2);
	static bool TryDeferScreenText(void* self, void* arg);
	static bool TryDeferUI(void* self, DWORD arg);

	// Re-invokes the captured calls in capture order (which is also the
	// game's draw order), then empties the buffer. Runs with the view
	// dimensions already restored. Records whose addresses are not
	// readable code (unit tests, a foreign host exe) are dropped.
	static void ReplayDeferred();

#ifdef VIEWCTRL_TEST
	// Record inspection for the unit tests.
	static Kind RecordedKind(int index);
	static const wchar_t* RecordedText(int index);
	static void RecordedTimer(int index, int& outIndex, void*& outScheme,
		int& outTime, void*& outXy1, void*& outXy2);
	static void RecordedScreenText(int index, void*& outSelf, void*& outArg);
	static void RecordedUI(int index, void*& outSelf, DWORD& outArg);

	// Simulates an in-flight replay: the hooks must pass through.
	static void SetReplayingForTest(bool replaying);

	// Replay probe: when set, ReplayDeferred calls it per record instead
	// of dispatching to the game (the game addresses hold no code in the
	// test process). The record is still intact during the probe.
	static void SetReplayProbeForTest(void (*probe)(int index, Kind kind));

	// Forces the gameCode gate open so a unit test can exercise the real
	// dispatch path (DrawTimer/DrawScreenText/DrawUI) against a fake
	// target instead of dropping the records. Pass false to restore the
	// normal IsGameReadable gate.
	static void SetForceGameCodeForTest(bool force);

	// Redirects the DrawTimer / DrawScreenText dispatch targets; nulls
	// restore the game addresses. Together with SetForceGameCodeForTest
	// the real dispatch branch runs against a target the test owns instead
	// of jumping at the (unmapped) game addresses.
	static void SetGameEntryPointsForTest(void* timer, void* screenText);
#endif
};

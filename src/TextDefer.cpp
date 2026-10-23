#include "TextDefer.hpp"
#include "RenderZoom.hpp"
#include "GameAddrs.hpp"
#include "Seh.h"

#include <cstring>

namespace
{
	// One captured HUD draw. Kinds share the slot (a frame holds a handful
	// of timers, a few screen texts and exactly one DrawUI pass) and every
	// field is written before the record is published, so stale members of
	// a previous kind never leak into this one.
	//
	// Cross-frame pointer stability: scheme, xy1, xy2, sctSelf, sctArg and
	// uiSelf are raw game-object pointers forwarded verbatim to the
	// re-invoked originals. They are only valid for the frame the record
	// lives in — the game owns them and does not free or relocate them
	// between capture and ReplayDeferred() in the same frame. A record
	// never outlives the frame (Reset() / BeginFrame() clear it), so a
	// dangling pointer across frames cannot happen by design. The timer
	// string is the only field copied, because a caller may hand over a
	// scratch buffer.
	constexpr int kMaxRecords = 64;
	constexpr int kMaxText = 128; // wchars incl. the terminator

	struct Record
	{
		TextDefer::Kind kind = TextDefer::Kind::Timer;

		// Kind::Timer: fastcall (index=ECX, scheme=EDX) + 4 stack args.
		int timerIndex = 0;
		void* scheme = nullptr;
		int time = 0;
		void* xy1 = nullptr;
		void* xy2 = nullptr;
		wchar_t text[kMaxText] = {};

		// Kind::ScreenText: thiscall this + stack arg (Point2D* member).
		void* sctSelf = nullptr;
		void* sctArg = nullptr;

		// Kind::UI: DrawUI virtual call — this (ESI) + the dword arg the
		// dispatcher already pushed at [esp+0x10].
		void* uiSelf = nullptr;
		DWORD uiArg = 0;
	};

	Record g_records[kMaxRecords];
	int g_count = 0;
	bool g_active = false;

	// Set while ReplayDeferred re-invokes the originals: the hooks must
	// pass through so the re-entered calls run (a fresh defer inside a
	// replay would feed itself forever).
	bool g_replaying = false;
	bool g_loggedOverflow = false;

	// Replays since the last heartbeat line: the first draw of a burst
	// logs immediately, then only every 60th replay — a magnified frame
	// with a visible timer would otherwise write the same line at the
	// frame rate.
	int g_sinceReplayLog = 0;

	// Copies the NUL-terminated wide string at src into dst. A null src
	// becomes an empty string — the timer still draws its backing, so it
	// still belongs on the replay. False when the pointer is unreadable
	// (SEH, release only) or the text outgrows the record buffer — the
	// draw is not captured and plays through untouched.
	bool CaptureText(wchar_t* dst, const wchar_t* src)
	{
		if (!src)
		{
			dst[0] = L'\0';
			return true;
		}

		bool done = false;
		size_t n = 0;
		VIEWCTRL_SEH_BEGIN
		{
			for (; n < (size_t)kMaxText - 1; ++n)
			{
				const wchar_t c = src[n];
				if (!c)
				{
					done = true;
					break;
				}
				dst[n] = c;
			}
			if (done)
				dst[n] = L'\0';
		}
		VIEWCTRL_SEH_END
		return done;
	}

	// Common head of the three Try* entry points: the frame must be armed
	// and not replaying, the buffer must have room.
	bool BeginCapture()
	{
		if (g_replaying || !g_active)
			return false;

		if (g_count >= kMaxRecords)
		{
			if (!g_loggedOverflow)
			{
				LOG("TextDefer: record buffer full (%d) — the rest of the frame stays stretched",
					kMaxRecords);
				g_loggedOverflow = true;
			}
			return false;
		}
		return true;
	}

	// The replay targets. DrawTimer/DrawScreenText are plain functions,
	// DrawUI goes through the object's vtable (slot 0x40 / 16) exactly
	// like the skipped dispatcher instructions did.
	using DrawTimerFn = int(__fastcall*)(int, void*, int, const wchar_t*,
		void*, void*);
	using DrawScreenTextFn = void(__thiscall*)(void*, void*);
	using DrawUIFn = void(__thiscall*)(void*, DWORD);

	// The dispatch targets behind the gameCode gate: the game addresses by
	// default, SetGameEntryPointsForTest (VIEWCTRL_TEST) redirects them so
	// a unit test can run the real dispatch branch against a target it owns
	// (the gate itself is opened by SetForceGameCodeForTest).
	DrawTimerFn g_drawTimerEntry = reinterpret_cast<DrawTimerFn>(
		GameAddr::TacticalClass_DrawTimer);
	DrawScreenTextFn g_drawScreenTextEntry = reinterpret_cast<DrawScreenTextFn>(
		GameAddr::TacticalClass_DrawScreenText);

#ifdef VIEWCTRL_TEST
	// Test-only replay probe (see SetReplayProbeForTest).
	void (*g_probe)(int index, TextDefer::Kind kind) = nullptr;
	// Test-only: force the gameCode gate open so a unit test can exercise
	// the real dispatch path (DrawTimer/DrawScreenText/DrawUI) against a
	// fake target instead of dropping the records.
	bool g_forceGameCode = false;
#endif
} // namespace

void TextDefer::BeginFrame(const RECT& view, int srcW, int srcH)
{
	(void)view;
	g_count = 0;
	g_active = srcW > 0 && srcH > 0;
	g_loggedOverflow = false;
}

void TextDefer::Reset()
{
	g_count = 0;
	g_active = false;
}

int TextDefer::RecordCount()
{
	return g_count;
}

bool TextDefer::TryDeferTimer(int index, void* scheme, int time,
	const wchar_t* text, void* xy1, void* xy2)
{
	if (!BeginCapture())
		return false;

	Record& rec = g_records[g_count];
	if (!CaptureText(rec.text, text))
		return false;

	rec.kind = Kind::Timer;
	rec.timerIndex = index;
	rec.scheme = scheme;
	rec.time = time;
	rec.xy1 = xy1;
	rec.xy2 = xy2;
	++g_count;
	return true;
}

bool TextDefer::TryDeferScreenText(void* self, void* arg)
{
	if (!self)
		return false;
	if (!BeginCapture())
		return false;

	Record& rec = g_records[g_count];
	rec.kind = Kind::ScreenText;
	rec.sctSelf = self;
	rec.sctArg = arg;
	++g_count;
	return true;
}

bool TextDefer::TryDeferUI(void* self, DWORD arg)
{
	if (!self)
		return false;
	if (!BeginCapture())
		return false;

	Record& rec = g_records[g_count];
	rec.kind = Kind::UI;
	rec.uiSelf = self;
	rec.uiArg = arg;
	++g_count;
	return true;
}

void TextDefer::ReplayDeferred()
{
	const int count = g_count;
	g_active = false;

	if (count <= 0)
	{
		// A quiet frame ends the burst: the next captured draw logs again.
		g_sinceReplayLog = 0;
		return;
	}
	if (g_replaying)
		return; // a nested call: the outer replay owns the buffer

#ifdef VIEWCTRL_TEST
	// The probe path runs for every record; without one the addresses
	// below hold no code in the test process and the records drop.
	const bool probeSet = g_probe != nullptr;
#else
	constexpr bool probeSet = false;
#endif

	// The addresses only hold real code inside gamemd.exe; a unit test
	// (or a foreign host exe) drops the records instead of jumping at
	// whatever lives there. The test override lets a unit test exercise
	// the real dispatch path against a fake vtable.
#ifdef VIEWCTRL_TEST
	const bool gameCode = g_forceGameCode
		|| (RenderZoom::IsGameReadable(reinterpret_cast<const void*>(GameAddr::TacticalClass_DrawTimer), 6)
			&& RenderZoom::IsGameReadable(reinterpret_cast<const void*>(GameAddr::TacticalClass_DrawScreenText), 5)
			&& RenderZoom::IsGameReadable(reinterpret_cast<const void*>(GameAddr::GScreenClass_Render_DrawUI), 12));
#else
	const bool gameCode =
		RenderZoom::IsGameReadable(reinterpret_cast<const void*>(GameAddr::TacticalClass_DrawTimer), 6)
		&& RenderZoom::IsGameReadable(reinterpret_cast<const void*>(GameAddr::TacticalClass_DrawScreenText), 5)
		&& RenderZoom::IsGameReadable(reinterpret_cast<const void*>(GameAddr::GScreenClass_Render_DrawUI), 12);
#endif
	if (!probeSet && !gameCode)
	{
		LOG("TextDefer: %d record(s) dropped outside the game", count);
		g_count = 0;
		return;
	}

	g_replaying = true;
	int replayed = 0;
	int nTimer = 0;
	int nSct = 0;
	int nUi = 0;
	for (int i = 0; i < count; ++i)
	{
		Record& rec = g_records[i];
		VIEWCTRL_SEH_BEGIN
#ifdef VIEWCTRL_TEST
		if (g_probe)
		{
			g_probe(i, rec.kind);
			++replayed;
		}
		else
#endif
		if (rec.kind == Kind::Timer)
		{
			if (gameCode)
			{
				g_drawTimerEntry(rec.timerIndex, rec.scheme, rec.time,
					rec.text, rec.xy1, rec.xy2);
				++replayed;
				++nTimer;
			}
		}
		else if (rec.kind == Kind::ScreenText)
		{
			if (gameCode)
			{
				g_drawScreenTextEntry(rec.sctSelf, rec.sctArg);
				++replayed;
				++nSct;
			}
		}
		else // Kind::UI
		{
			if (gameCode)
			{
				// Defensive: the vtable pointer and the DrawUI slot
				// (0x40 / 16) must be readable before we dispatch. A
				// dangling or foreign `this` must not jump through
				// garbage; the record simply drops.
				void** vtable = nullptr;
				void* slot = nullptr;
				VIEWCTRL_SEH_BEGIN
				vtable = *static_cast<void***>(rec.uiSelf);
				if (vtable)
					slot = vtable[16];
				VIEWCTRL_SEH_END
				if (slot)
				{
					const auto fn = reinterpret_cast<DrawUIFn>(slot);
					fn(rec.uiSelf, rec.uiArg);
					++replayed;
					++nUi;
				}
			}
		}
		VIEWCTRL_SEH_END
	}
	g_replaying = false;
	// The records stayed live for the whole pass (a test probe reads them
	// through the Recorded* accessors); they die with the replay now.
	g_count = 0;

	if (g_sinceReplayLog == 0 || ++g_sinceReplayLog > 60)
	{
		LOG("TextDefer: replayed %d/%d draw(s) (timer=%d text=%d ui=%d)",
			replayed, count, nTimer, nSct, nUi);
		g_sinceReplayLog = 1;
	}
}

#ifdef VIEWCTRL_TEST
TextDefer::Kind TextDefer::RecordedKind(int index)
{
	return (index >= 0 && index < g_count) ? g_records[index].kind
		: Kind::Timer;
}

const wchar_t* TextDefer::RecordedText(int index)
{
	return (index >= 0 && index < g_count) ? g_records[index].text : nullptr;
}

void TextDefer::RecordedTimer(int index, int& outIndex, void*& outScheme,
	int& outTime, void*& outXy1, void*& outXy2)
{
	outIndex = 0;
	outScheme = nullptr;
	outTime = 0;
	outXy1 = nullptr;
	outXy2 = nullptr;
	if (index >= 0 && index < g_count && g_records[index].kind == Kind::Timer)
	{
		const Record& rec = g_records[index];
		outIndex = rec.timerIndex;
		outScheme = rec.scheme;
		outTime = rec.time;
		outXy1 = rec.xy1;
		outXy2 = rec.xy2;
	}
}

void TextDefer::RecordedScreenText(int index, void*& outSelf, void*& outArg)
{
	outSelf = nullptr;
	outArg = nullptr;
	if (index >= 0 && index < g_count && g_records[index].kind == Kind::ScreenText)
	{
		outSelf = g_records[index].sctSelf;
		outArg = g_records[index].sctArg;
	}
}

void TextDefer::RecordedUI(int index, void*& outSelf, DWORD& outArg)
{
	outSelf = nullptr;
	outArg = 0;
	if (index >= 0 && index < g_count && g_records[index].kind == Kind::UI)
	{
		outSelf = g_records[index].uiSelf;
		outArg = g_records[index].uiArg;
	}
}

void TextDefer::SetReplayingForTest(bool replaying)
{
	g_replaying = replaying;
}

void TextDefer::SetReplayProbeForTest(void (*probe)(int index, Kind kind))
{
	g_probe = probe;
}

void TextDefer::SetForceGameCodeForTest(bool force)
{
	g_forceGameCode = force;
}

void TextDefer::SetGameEntryPointsForTest(void* timer, void* screenText)
{
	g_drawTimerEntry = timer
		? reinterpret_cast<DrawTimerFn>(timer)
		: reinterpret_cast<DrawTimerFn>(GameAddr::TacticalClass_DrawTimer);
	g_drawScreenTextEntry = screenText
		? reinterpret_cast<DrawScreenTextFn>(screenText)
		: reinterpret_cast<DrawScreenTextFn>(GameAddr::TacticalClass_DrawScreenText);
}
#endif

// Keyboard and mouse for Minecraft while it drives the player.
//
// The game window is subclassed (SetWindowLongPtrW(GWLP_WNDPROC)) once rage::g_pHWND exists:
// WM_KEYDOWN/UP (hardware scancode -> DirectInput code -> SDL3 scancode), WM_CHAR (kInText while a
// Minecraft screen is open), mouse buttons and wheel, focus changes (kInReleaseAll). Relative
// mouse motion comes from raw input (WM_INPUT, RIDEV_INPUTSINK): look deltas for Game, or the
// virtual GUI cursor (kInCursor) while a Minecraft screen is open.
// While puppeting, only Esc, F1-F12 and ` reach the game's window procedure; but GTA IV reads the
// keyboard and mouse through DirectInput/CPad as well, so Pad() also zeroes the player's pad.
#pragma once

#include <cstdint>

class CPad;  // IV-SDK

namespace lc::Input
{
	// Every frame from Game::Tick: subclasses the window once it exists. Idempotent.
	void Install();
	// Restores the window procedure and the previous raw-input registration. a_quiet: no logging
	// (at process exit other threads may have died holding the log's lock).
	void Uninstall(bool a_quiet = false);
	// Raw mouse registration follows puppet mode (taken while puppeting, restored after).
	void SetCapture(bool a_capture);
	// processPadEvent: while puppeting, zero every control except the pause menu's.
	void Pad(CPad* a_pad);
	// Mouse-look counts accumulated since the last call (raw mouse units).
	void ConsumeLook(float& a_dx, float& a_dy);
	// kInReleaseAll (focus left Minecraft). Safe from any thread.
	void ReleaseAll();

	struct Counters
	{
		std::uint32_t keys, buttons, scrolls, chars, cursors, rawMouse, releaseAll, openMenu, dropped, padZeroed;
	};
	// Counts since the last call.
	Counters TakeCounters();
}

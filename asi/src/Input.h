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
	// Every frame from Game::Tick (game thread). While captured, takes the raw mouse back when
	// something else registered it: Wine's DirectInput re-registers it for its own window whenever
	// GTA IV (re)acquires its mouse, e.g. after alt-tab, which left mouse look dead. Also runs the
	// DebugFocusCycle test hook.
	void Tick(float a_dt);
	// GTA's phone is out (taken out by the player, or a call): from the game's own flag, updated by
	// Tick. Safe from any thread.
	bool PhoneOut();
	// GTA's help text offers context actions or menu choices (Missions.h; drive::PromptAction bits): their pad
	// controls are let through while puppeting, and their keys (ContextKey, Enter, Backspace) go to GTA
	// instead of Minecraft. Game thread.
	void SetPrompt(unsigned a_actions);
	// processPadEvent: while puppeting (or Niko gets back up, Game::Shared::padLocked), zero every
	// control except the pause menu's, the phone's and an offered context action's.
	void Pad(CPad* a_pad);
	// Test hooks: a real key event (SendInput, a DirectInput scan code).
	void SendTestKey(std::uint32_t a_dik, bool a_down);
	// Mouse-look counts accumulated since the last call (raw mouse units).
	void ConsumeLook(float& a_dx, float& a_dy);
	// DebugInputScript's look turns ("v" entries): degrees of Minecraft yaw and pitch (down positive).
	// True: an "a" entry, the look set to them instead.
	bool ConsumeScriptLook(float& a_yaw, float& a_pitch);
	// kInReleaseAll (focus left Minecraft). Safe from any thread.
	void ReleaseAll();

	struct Counters
	{
		std::uint32_t keys, buttons, scrolls, chars, cursors, rawMouse, releaseAll, openMenu, dropped, padZeroed, rawRetaken;
	};
	// Counts since the last call.
	Counters TakeCounters();
}

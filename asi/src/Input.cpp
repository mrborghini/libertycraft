// Unity-built into dllmain.cpp (needs IV-SDK). See Input.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "input"
#include "Input.h"

#include "Config.h"
#include "Game.h"
#include "HostDrive.h"
#include "Link.h"
#include "Log.h"
#include "Perf.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace lc::Input
{
	namespace
	{
		// DirectInput (set 1) scan code -> SDL3 scancode / USB HID usage (what Minecraft's GLFW
		// mapping on the Java side expects). Same table as SkyCraft's Input.cpp.
		constexpr auto kDikToSdl = [] {
			std::array<std::uint16_t, 256> t{};
			t[0x01] = 41;  // Esc
			for (int i = 0; i < 9; ++i) t[0x02 + i] = static_cast<std::uint16_t>(30 + i);  // 1-9
			t[0x0B] = 39;  // 0
			t[0x0C] = 45, t[0x0D] = 46, t[0x0E] = 42, t[0x0F] = 43;  // - = Backspace Tab
			t[0x10] = 20, t[0x11] = 26, t[0x12] = 8, t[0x13] = 21, t[0x14] = 23;  // Q W E R T
			t[0x15] = 28, t[0x16] = 24, t[0x17] = 12, t[0x18] = 18, t[0x19] = 19;  // Y U I O P
			t[0x1A] = 47, t[0x1B] = 48, t[0x1C] = 40, t[0x1D] = 224;               // [ ] Enter LCtrl
			t[0x1E] = 4, t[0x1F] = 22, t[0x20] = 7, t[0x21] = 9, t[0x22] = 10;     // A S D F G
			t[0x23] = 11, t[0x24] = 13, t[0x25] = 14, t[0x26] = 15;                // H J K L
			t[0x27] = 51, t[0x28] = 52, t[0x29] = 53, t[0x2A] = 225, t[0x2B] = 49;  // ; ' ` LShift backslash
			t[0x2C] = 29, t[0x2D] = 27, t[0x2E] = 6, t[0x2F] = 25, t[0x30] = 5;    // Z X C V B
			t[0x31] = 17, t[0x32] = 16, t[0x33] = 54, t[0x34] = 55, t[0x35] = 56;  // N M , . /
			t[0x36] = 229, t[0x37] = 85, t[0x38] = 226, t[0x39] = 44, t[0x3A] = 57;  // RShift KP* LAlt Space Caps
			for (int i = 0; i < 10; ++i) t[0x3B + i] = static_cast<std::uint16_t>(58 + i);  // F1-F10
			t[0x45] = 83, t[0x46] = 71;                                             // NumLock ScrollLock
			t[0x47] = 95, t[0x48] = 96, t[0x49] = 97, t[0x4A] = 86;                 // KP7 KP8 KP9 KP-
			t[0x4B] = 92, t[0x4C] = 93, t[0x4D] = 94, t[0x4E] = 87;                 // KP4 KP5 KP6 KP+
			t[0x4F] = 89, t[0x50] = 90, t[0x51] = 91, t[0x52] = 98, t[0x53] = 99;   // KP1 KP2 KP3 KP0 KP.
			t[0x56] = 100, t[0x57] = 68, t[0x58] = 69;                              // OEM102 F11 F12
			t[0x9C] = 88, t[0x9D] = 228, t[0xB5] = 84, t[0xB7] = 70, t[0xB8] = 230;  // KPEnter RCtrl KP/ PrtSc RAlt
			t[0xC5] = 72, t[0xC7] = 74, t[0xC8] = 82, t[0xC9] = 75, t[0xCB] = 80;  // Pause Home Up PgUp Left
			t[0xCD] = 79, t[0xCF] = 77, t[0xD0] = 81, t[0xD1] = 78, t[0xD2] = 73;  // Right End Down PgDn Insert
			t[0xD3] = 76, t[0xDB] = 227, t[0xDC] = 231, t[0xDD] = 101;             // Delete LWin RWin Menu
			return t;
		}();

		constexpr std::uint32_t kDikEscape = 0x01;
		constexpr std::uint32_t kDikGrave = 0x29;  // ` / ~

		bool IsFunctionKey(std::uint32_t a_dik) { return (a_dik >= 0x3B && a_dik <= 0x44) || a_dik == 0x57 || a_dik == 0x58; }
		// Keys the game's window procedure still gets while Minecraft drives the player.
		bool IsGameKey(std::uint32_t a_dik) { return a_dik == kDikEscape || a_dik == kDikGrave || IsFunctionKey(a_dik); }

		// ---- GTA's phone (PhoneKeys) ------------------------------------------------------------------
		// The arrow keys are always GTA's (Up takes the phone out or answers a call, the arrows
		// navigate); while the phone is out Enter, Backspace and the number keys are GTA's too
		// (select, back, dialling). Minecraft gets none of them then, except key-ups (it may have
		// seen the key go down before the phone came out).
		std::atomic<bool> phoneOut{ false };

		bool IsArrowKey(std::uint32_t a_dik) { return a_dik == 0xC8 || a_dik == 0xD0 || a_dik == 0xCB || a_dik == 0xCD; }
		bool IsPhoneKey(std::uint32_t a_dik)
		{
			if (!Config::Get().phoneKeys) {
				return false;
			}
			if (IsArrowKey(a_dik)) {
				return true;
			}
			if (!phoneOut.load(std::memory_order_relaxed)) {
				return false;
			}
			const bool digit = (a_dik >= 0x02 && a_dik <= 0x0B) || (a_dik >= 0x47 && a_dik <= 0x53 && a_dik != 0x4A && a_dik != 0x4E);
			return a_dik == 0x1C || a_dik == 0x9C || a_dik == 0x0E || digit;  // Enter, keypad Enter, Backspace, 0 to 9
		}

		// The pad controls GTA's phone reads, kept while puppet mode zeroes the pad. Measured in game
		// (DebugPhone): Up feeds PHONE_TAKE_OUT, FRONTEND_UP and KB_UP; the other arrows FRONTEND_ and
		// KB_ DOWN/LEFT/RIGHT; Enter FRONTEND_ACCEPT and KB_PHONE_ACCEPT; Backspace FRONTEND_CANCEL and
		// KB_PHONE_CANCEL (mouse buttons feed nothing while puppet mode holds the raw mouse). The
		// arrows' controls and the phone's own accept/cancel always (an incoming call is answered or
		// declined before the phone is out; nothing but the phone reads them), the frontend
		// accept/cancel only while the phone is out.
		bool IsPhoneControl(int a_control, bool a_out)
		{
			switch (a_control) {
			case INPUT_PHONE_TAKE_OUT:
			case INPUT_PHONE_PUT_AWAY:
			case INPUT_KB_UP:
			case INPUT_KB_DOWN:
			case INPUT_KB_LEFT:
			case INPUT_KB_RIGHT:
			case INPUT_FRONTEND_UP:
			case INPUT_FRONTEND_DOWN:
			case INPUT_FRONTEND_LEFT:
			case INPUT_FRONTEND_RIGHT:
			case INPUT_KB_PHONE_ACCEPT:
			case INPUT_KB_PHONE_CANCEL:
				return true;
			case INPUT_FRONTEND_ACCEPT:
			case INPUT_FRONTEND_CANCEL:
				return a_out;
			default:
				return false;
			}
		}

		HWND                       window = nullptr;
		WNDPROC                    originalProc = nullptr;
		bool                       installLogged = false;
		std::atomic<bool>          focused{ true };
		std::atomic<std::int32_t>  lookDx{ 0 }, lookDy{ 0 };
		wchar_t                    highSurrogate = 0;
		// raw input
		bool                       captured = false;
		bool                       hadPrevious = false;
		RAWINPUTDEVICE             previous{};
		bool                       quiet = false;  // set while unloading
		std::atomic<std::uint32_t> focusChanges{ 0 };   // SetFocus flips (window thread), for the watchdog
		std::uint32_t              focusChangesSeen = 0;
		float                      watchdogT = 0.0f;   // seconds until the next routine check
		float                      watchdogBurstT = 0.0f;  // after a focus change: check every frame this long
		std::uint32_t              retakes = 0;
		bool                       watchdogOff = false;  // DebugFocusCycle: its first cycle runs without it
		std::atomic<std::uint32_t> rawMouseTotal{ 0 };   // WM_INPUT mouse motion ever received (DebugFocusCycle)

		struct AtomicCounters
		{
			std::atomic<std::uint32_t> keys{ 0 }, buttons{ 0 }, scrolls{ 0 }, chars{ 0 }, cursors{ 0 }, rawMouse{ 0 }, releaseAll{ 0 }, openMenu{ 0 },
				dropped{ 0 }, padZeroed{ 0 }, rawRetaken{ 0 };
		} counters;

		void Push(proto::InputType a_type, std::uint16_t a_code, std::int32_t a_a = 0, std::int32_t a_b = 0)
		{
			if (!Link::Get().PushInput(a_type, a_code, a_a, a_b)) {
				counters.dropped.fetch_add(1, std::memory_order_relaxed);
			}
		}

		// lParam of WM_KEYDOWN/UP -> DirectInput code (scan code, +0x80 for extended keys).
		std::uint32_t DikFromKeyMessage(WPARAM a_vk, LPARAM a_lParam)
		{
			std::uint32_t scan = (static_cast<std::uint32_t>(a_lParam) >> 16) & 0xFF;
			const bool    extended = (a_lParam >> 24) & 1;
			if (scan == 0) {  // synthesised keystrokes may carry no scan code
				const UINT sc = ::MapVirtualKeyW(static_cast<UINT>(a_vk), MAPVK_VK_TO_VSC_EX);
				scan = sc & 0xFF;
				return scan | ((sc & 0xFF00) ? 0x80u : 0u);
			}
			if (scan == 0x45) {
				return extended ? 0x45 : 0xC5;  // NumLock comes extended, Pause doesn't: the reverse of DIK
			}
			return scan | (extended ? 0x80u : 0u);
		}

		bool Routing()
		{
			const auto& st = Game::State();
			return st.puppeting.load(std::memory_order_relaxed) && !st.gtaMenuOpen.load(std::memory_order_relaxed) && focused.load(std::memory_order_relaxed);
		}

		void OnRawInput(HRAWINPUT a_handle)
		{
			alignas(8) std::uint8_t buffer[sizeof(RAWINPUT) + 64];
			UINT                    size = sizeof(buffer);
			if (::GetRawInputData(a_handle, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
				return;
			}
			const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer);
			if (raw->header.dwType != RIM_TYPEMOUSE || (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
				return;
			}
			const LONG dx = raw->data.mouse.lLastX, dy = raw->data.mouse.lLastY;
			if (!dx && !dy) {
				return;
			}
			counters.rawMouse.fetch_add(1, std::memory_order_relaxed);
			rawMouseTotal.fetch_add(1, std::memory_order_relaxed);
			if (!Routing()) {
				return;
			}
			auto& st = Game::State();
			if (st.mcScreenOpen.load(std::memory_order_relaxed)) {
				const int w = std::max(1, st.viewportW.load()), h = std::max(1, st.viewportH.load());
				const int x = std::clamp(st.cursorX.load() + static_cast<int>(dx), 0, w - 1);
				const int y = std::clamp(st.cursorY.load() + static_cast<int>(dy), 0, h - 1);
				st.cursorX = x;
				st.cursorY = y;
				Push(proto::kInCursor, 0, x, y);
				counters.cursors.fetch_add(1, std::memory_order_relaxed);
			} else {
				lookDx.fetch_add(dx, std::memory_order_relaxed);
				lookDy.fetch_add(dy, std::memory_order_relaxed);
			}
		}

		// Returns true if the message was consumed (not passed to the game).
		bool OnKey(UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
		{
			const bool down = a_msg == WM_KEYDOWN || a_msg == WM_SYSKEYDOWN;
			const bool repeat = down && ((a_lParam >> 30) & 1);
			const auto dik = DikFromKeyMessage(a_wParam, a_lParam);
			if (focused.load(std::memory_order_relaxed) && HostDrive::OnKey(dik, down, repeat)) {
				return true;  // the toggle key (any mode) or the vehicle key (taken from Minecraft)
			}
			if (!Routing()) {
				return false;
			}
			const bool screenOpen = Game::State().mcScreenOpen.load(std::memory_order_relaxed);
			if (!screenOpen) {
				if (dik == Config::Get().MenuKeyDik() && dik != 0) {
					if (down && !repeat) {
						ReleaseAll();
						Push(proto::kInOpenMenu, 0);
						counters.openMenu.fetch_add(1, std::memory_order_relaxed);
					}
					return true;
				}
				if (dik == kDikEscape || dik == kDikGrave) {
					return false;  // the game's (pause menu, console mods)
				}
				if (IsPhoneKey(dik)) {
					if (!down) {
						if (const auto sdl = kDikToSdl[dik & 0xFF]) {
							Push(proto::kInKey, sdl, 0);
						}
					}
					return false;  // GTA's phone
				}
			}
			// Held keys repeat only into Minecraft screens (text fields); in the world MC keeps state.
			if (repeat && !screenOpen) {
				return !IsGameKey(dik);
			}
			if (const auto sdl = kDikToSdl[dik & 0xFF]) {
				Push(proto::kInKey, sdl, down ? 1 : 0);
				counters.keys.fetch_add(1, std::memory_order_relaxed);
			}
			// With a Minecraft screen up every key is Minecraft's (Esc closes the screen); otherwise
			// the function keys also reach the game.
			return screenOpen || !IsFunctionKey(dik);
		}

		bool OnButton(UINT a_msg, WPARAM a_wParam)
		{
			if (!Routing()) {
				return false;
			}
			std::uint16_t button = 0;
			bool          down = false;
			switch (a_msg) {
			case WM_LBUTTONDOWN: button = 1, down = true; break;
			case WM_LBUTTONUP: button = 1; break;
			case WM_MBUTTONDOWN: button = 2, down = true; break;
			case WM_MBUTTONUP: button = 2; break;
			case WM_RBUTTONDOWN: button = 3, down = true; break;
			case WM_RBUTTONUP: button = 3; break;
			case WM_XBUTTONDOWN: button = HIWORD(a_wParam) == XBUTTON1 ? 4 : 5, down = true; break;
			case WM_XBUTTONUP: button = HIWORD(a_wParam) == XBUTTON1 ? 4 : 5; break;
			default: return false;
			}
			Push(proto::kInMouseButton, button, down ? 1 : 0);
			counters.buttons.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		void SetFocus(bool a_focused, const char* a_why)
		{
			if (focused.exchange(a_focused) != a_focused) {
				LC_LOG("window %s (%s)", a_focused ? "focused" : "lost focus", a_why);
				focusChanges.fetch_add(1, std::memory_order_relaxed);
				if (!a_focused) {
					ReleaseAll();
				}
			}
		}

		LRESULT CALLBACK WndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
		{
			switch (a_msg) {
			case WM_INPUT:
				OnRawInput(reinterpret_cast<HRAWINPUT>(a_lParam));
				break;  // always passed on: DefWindowProc must clean up raw input
			case WM_KEYDOWN:
			case WM_KEYUP:
			case WM_SYSKEYDOWN:
			case WM_SYSKEYUP:
				if (OnKey(a_msg, a_wParam, a_lParam)) {
					return 0;
				}
				break;
			case WM_CHAR:
				if (Routing() && !Game::State().mcScreenOpen.load(std::memory_order_relaxed)) {
					// In the world, typed characters are nobody's; ` and ~ stay the game's (console mods).
					if (a_wParam == L'`' || a_wParam == L'~') {
						break;
					}
					return 0;
				}
				if (Routing()) {
					const auto c = static_cast<wchar_t>(a_wParam);
					if (c >= 0xD800 && c < 0xDC00) {
						highSurrogate = c;
					} else {
						std::int32_t cp = c;
						if (c >= 0xDC00 && c < 0xE000 && highSurrogate) {
							cp = 0x10000 + ((highSurrogate - 0xD800) << 10) + (c - 0xDC00);
						}
						highSurrogate = 0;
						if (cp >= 0x20 && cp != 0x7F) {  // control characters arrive as keys
							Push(proto::kInText, 0, cp);
							counters.chars.fetch_add(1, std::memory_order_relaxed);
						}
					}
					return 0;
				}
				break;
			case WM_LBUTTONDOWN:
			case WM_LBUTTONUP:
			case WM_MBUTTONDOWN:
			case WM_MBUTTONUP:
			case WM_RBUTTONDOWN:
			case WM_RBUTTONUP:
			case WM_XBUTTONDOWN:
			case WM_XBUTTONUP:
				if (OnButton(a_msg, a_wParam)) {
					return a_msg == WM_XBUTTONDOWN || a_msg == WM_XBUTTONUP ? TRUE : 0;
				}
				break;
			case WM_MOUSEWHEEL:
				if (Routing()) {
					Push(proto::kInScroll, 0, GET_WHEEL_DELTA_WPARAM(a_wParam));
					counters.scrolls.fetch_add(1, std::memory_order_relaxed);
					return 0;
				}
				break;
			case WM_ACTIVATEAPP:
				SetFocus(a_wParam != FALSE, "WM_ACTIVATEAPP");
				break;
			case WM_KILLFOCUS:
				SetFocus(false, "WM_KILLFOCUS");
				break;
			case WM_SETFOCUS:
				SetFocus(true, "WM_SETFOCUS");
				break;
			default:
				break;
			}
			return ::CallWindowProcW(originalProc, a_hwnd, a_msg, a_wParam, a_lParam);
		}

		void LogRawInputRegistrations()
		{
			UINT count = 0;
			if (::GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1) && count == 0) {
				return;
			}
			std::vector<RAWINPUTDEVICE> devices(count);
			if (count == 0 || ::GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1)) {
				LC_LOG("raw input: the game has no raw input registrations");
				return;
			}
			for (const auto& d : devices) {
				LC_LOG("raw input: existing registration usage %u/%u flags 0x%lX hwnd %p%s", d.usUsagePage, d.usUsage, d.dwFlags, static_cast<void*>(d.hwndTarget),
					d.hwndTarget == window ? " (the game window)" : "");
			}
		}

		// The process's raw mouse registration (one per usage per process), if any.
		bool FindMouseRegistration(RAWINPUTDEVICE& a_out)
		{
			RAWINPUTDEVICE devices[16];
			UINT           count = 16;
			const UINT     n = ::GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
			if (n == static_cast<UINT>(-1)) {
				return false;
			}
			for (UINT i = 0; i < n; ++i) {
				if (devices[i].usUsagePage == 0x01 && devices[i].usUsage == 0x02) {
					a_out = devices[i];
					return true;
				}
			}
			return false;
		}

		// Raw input keeps one mouse registration per process, and Wine's DirectInput (re)registers it
		// for its own input window (RIDEV_CAPTUREMOUSE | RIDEV_NOLEGACY, flags 0x230) whenever GTA IV
		// acquires its mouse, and removes it when GTA unacquires: alt-tab does both. Puppet mode's
		// registration was gone afterwards, so no WM_INPUT reached the game window (no mouse look) and,
		// with NOLEGACY, no mouse button messages either, until puppet mode restarted (a trip through
		// GTA's pause menu did that). While captured and focused, take it back.
		void Watchdog(float a_dt)
		{
			if (!captured || !window || watchdogOff) {
				return;
			}
			const std::uint32_t changes = focusChanges.load(std::memory_order_relaxed);
			if (changes != focusChangesSeen) {
				focusChangesSeen = changes;
				watchdogBurstT = 3.0f;  // GTA re-acquires its mouse some frames after the focus comes back
			}
			if (!focused.load(std::memory_order_relaxed)) {
				return;  // (the game loop is blocked while unfocused anyway)
			}
			watchdogBurstT = std::max(0.0f, watchdogBurstT - a_dt);
			if ((watchdogT -= a_dt) > 0.0f && watchdogBurstT <= 0.0f) {
				return;
			}
			watchdogT = 0.1f;
			RAWINPUTDEVICE current{};
			const bool     found = FindMouseRegistration(current);
			if (found && current.hwndTarget == window && (current.dwFlags & RIDEV_INPUTSINK)) {
				return;
			}
			if (found) {
				previous = current;  // whoever registered it last gets it back when puppet mode ends
				hadPrevious = true;
			}
			RAWINPUTDEVICE mouse{ 0x01, 0x02, RIDEV_INPUTSINK, window };
			const bool     ok = ::RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) != FALSE;
			counters.rawRetaken.fetch_add(1, std::memory_order_relaxed);
			if (++retakes <= 20) {
				LC_LOG("raw mouse: the registration was %s (%s hwnd %p flags 0x%lX); %s for the game window", found ? "taken over" : "removed",
					found ? "now" : "was", static_cast<void*>(found ? current.hwndTarget : nullptr), found ? current.dwFlags : 0ul,
					ok ? "registered it again" : "re-registering FAILED");
			} else {
				LC_LOG_EVERY(10000, "raw mouse: taken back again (%u times so far)", retakes);
			}
		}

		// ---- DebugFocusCycle (test hook): alt-tab without a keyboard ------------------------------------
		// A window of our own on another thread takes the foreground for 3 s, then gives it back, like
		// alt-tab (WM_ACTIVATEAPP, WM_KILLFOCUS; Wine's DirectInput unacquires and re-acquires GTA's
		// mouse). Relative mouse moves injected with SendInput before and after show whether raw mouse
		// input still reaches the game window. The first cycle runs without the watchdog (the old
		// behaviour), then turns it on; the second has it on throughout.
		struct FocusTest
		{
			int               cycle = 0;
			int               phase = 0;  // 0 waiting, 1 probe before, 2 focus away, 3 after
			float             t = 0.0f;
			std::uint32_t     rawAt = 0;
			std::atomic<bool> thiefDone{ false };
		} focusTest;

		void InjectMouseMoves()
		{
			for (int i = 0; i < 5; ++i) {
				INPUT in{};
				in.type = INPUT_MOUSE;
				in.mi.dx = (i % 2) ? -3 : 3;
				in.mi.dwFlags = MOUSEEVENTF_MOVE;
				::SendInput(1, &in, sizeof(in));
			}
		}

		void LogProbe(const char* a_when)
		{
			RAWINPUTDEVICE current{};
			const bool     found = FindMouseRegistration(current);
			LC_LOG("DebugFocusCycle %d: %s: %u raw mouse messages for 5 injected moves; mouse registration: %s hwnd %p flags 0x%lX%s; watchdog %s",
				focusTest.cycle + 1, a_when, rawMouseTotal.load() - focusTest.rawAt, found ? "" : "none,", static_cast<void*>(found ? current.hwndTarget : nullptr),
				found ? current.dwFlags : 0ul, found && current.hwndTarget == window ? " (the game window)" : "", watchdogOff ? "OFF" : "on");
		}

		DWORD WINAPI FocusThief(LPVOID)
		{
			HINSTANCE  inst = ::GetModuleHandleW(nullptr);
			WNDCLASSW  wc{};
			wc.lpfnWndProc = ::DefWindowProcW;
			wc.hInstance = inst;
			wc.lpszClassName = L"LibertyCraftFocusTest";
			::RegisterClassW(&wc);
			HWND w = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"LibertyCraft focus test", WS_POPUP | WS_VISIBLE, 0, 0, 32, 32,
				nullptr, nullptr, inst, nullptr);
			const BOOL took = w ? ::SetForegroundWindow(w) : FALSE;
			LC_LOG("DebugFocusCycle: test window %p takes the foreground (SetForegroundWindow %d; foreground now %p, game window %p)", static_cast<void*>(w), took,
				static_cast<void*>(::GetForegroundWindow()), static_cast<void*>(window));
			const auto pump = [](DWORD a_ms) {
				const ULONGLONG end = ::GetTickCount64() + a_ms;
				while (::GetTickCount64() < end) {
					MSG m;
					while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
						::TranslateMessage(&m);
						::DispatchMessageW(&m);
					}
					::Sleep(10);
				}
			};
			pump(1500);
			RAWINPUTDEVICE current{};
			const bool     found = FindMouseRegistration(current);
			LC_LOG("DebugFocusCycle: while away: foreground %p, mouse registration: %s hwnd %p flags 0x%lX", static_cast<void*>(::GetForegroundWindow()),
				found ? "" : "none,", static_cast<void*>(found ? current.hwndTarget : nullptr), found ? current.dwFlags : 0ul);
			pump(1500);
			const BOOL back = ::SetForegroundWindow(window);
			if (w) {
				::DestroyWindow(w);
			}
			pump(300);
			LC_LOG("DebugFocusCycle: foreground given back (SetForegroundWindow %d; foreground now %p, game window %p)", back,
				static_cast<void*>(::GetForegroundWindow()), static_cast<void*>(window));
			focusTest.thiefDone = true;
			return 0;
		}

		// ---- GTA's phone state ------------------------------------------------------------------------
		// CREATE_MOBILE_PHONE / DESTROY_MOBILE_PHONE (the phone scripts take it out and put it away,
		// for the player's own use and for calls) set a byte the game's own code checks (1.0.8.0:
		// 0x1792CB5, written at 0x4BF967); SCRIPT_IS_USING_MOBILE_PHONE sets the next one (0x4C05AD).
		// Their addresses are read from those instructions, which are checked first.
		const volatile std::uint8_t* phoneFlags = nullptr;
		bool                         phoneFlagsChecked = false;
		int                          lastPhoneState = -1;

		const volatile std::uint8_t* PhoneFlags()
		{
			if (!phoneFlagsChecked) {
				phoneFlagsChecked = true;
				if (plugin::gameVer == plugin::VERSION_1080) {
					const auto*   base = reinterpret_cast<const std::uint8_t*>(AddressSetter::gBaseAddress);
					std::uint32_t created = 0, using_ = 0;
					std::memcpy(&created, base + 0xBF969, 4);
					std::memcpy(&using_, base + 0xC05AE, 4);
					if (base[0xBF967] == 0x88 && base[0xBF968] == 0x1D && base[0xC05AD] == 0xA2 && using_ == created + 1) {
						phoneFlags = reinterpret_cast<const volatile std::uint8_t*>(static_cast<std::uintptr_t>(created));
					}
				}
				LC_LOG("phone: %s", phoneFlags ? "the game's phone flags found (PhoneKeys follow the phone)" : "WARNING: the phone flags aren't where 1.0.8.0 has them; Enter/Backspace stay Minecraft's");
			}
			return phoneFlags;
		}

		void UpdatePhone()
		{
			const volatile std::uint8_t* f = PhoneFlags();
			const bool created = f && f[0] != 0, using_ = f && f[1] != 0;
			const bool out = created || using_;
			if (out != phoneOut.load(std::memory_order_relaxed)) {
				phoneOut.store(out, std::memory_order_relaxed);
			}
			const int state = (created ? 1 : 0) | (using_ ? 2 : 0);
			if (state != lastPhoneState) {
				int ped = 0, sub = -1;
				::Scripting::GET_PLAYER_CHAR(static_cast<int>(::Scripting::GET_PLAYER_ID()), &ped);
				const bool task = ped && ::Scripting::GET_MOBILE_PHONE_TASK_SUB_TASK(ped, &sub);
				LC_LOG("phone: %s (phone created %d, script using it %d, on screen %d, Niko's phone task %d sub %d)", out ? "out" : "away", created, using_,
					static_cast<int>(::Scripting::CAN_PHONE_BE_SEEN_ON_SCREEN()), static_cast<int>(task), sub);
				lastPhoneState = state;
			}
		}

		// ---- DebugPhone (test hook) ------------------------------------------------------------------
		// Real key events (SendInput, through Wine's DirectInput like a keyboard): first a probe of which
		// pad controls each key feeds (logged), then the phone: Up takes it out, Enter opens the first
		// entry (the phone book), Down twice scrolls, Backspace backs out and puts it away.
		constexpr std::uint32_t kMouseLeft = 0x100, kMouseRight = 0x101, kWheelUp = 0x102, kWheelDown = 0x103;

		void SendKey(std::uint32_t a_key, bool a_down)
		{
			INPUT in{};
			if (a_key >= 0x100) {
				in.type = INPUT_MOUSE;
				switch (a_key) {
				case kMouseLeft: in.mi.dwFlags = a_down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
				case kMouseRight: in.mi.dwFlags = a_down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
				default:
					if (!a_down) {
						return;
					}
					in.mi.dwFlags = MOUSEEVENTF_WHEEL;
					in.mi.mouseData = static_cast<DWORD>(a_key == kWheelUp ? WHEEL_DELTA : -WHEEL_DELTA);
					break;
				}
			} else {
				in.type = INPUT_KEYBOARD;
				in.ki.wScan = static_cast<WORD>(a_key & 0x7F);
				in.ki.dwFlags = KEYEVENTF_SCANCODE | ((a_key & 0x80) ? KEYEVENTF_EXTENDEDKEY : 0) | (a_down ? 0 : KEYEVENTF_KEYUP);
			}
			::SendInput(1, &in, sizeof(in));
		}

		struct PhoneTest
		{
			struct Step
			{
				char          kind;  // 'p' probe a key, 't' tap a key, 'w' wait, 'l' log the state (a screenshot moment)
				std::uint32_t key;
				float         seconds;
				const char*   label;
			};
			int   step = -1;  // -1 waiting for puppet mode
			int   sub = 0;
			float t = 0.0f;
			bool  capture = false;  // Pad: record the raw pad
			std::array<std::uint8_t, 192> base{}, raw{};
			std::array<bool, 192>         changed{};
		} phoneTest;

		const PhoneTest::Step kPhoneSteps[] = {
			{ 'p', 0x11, 0, "W" }, { 'p', 0x02, 0, "1" }, { 'p', 0x1C, 0, "Enter" }, { 'p', 0x0E, 0, "Backspace" }, { 'p', kMouseLeft, 0, "left mouse" },
			{ 'p', 0xCB, 0, "Left" }, { 'p', 0xCD, 0, "Right" }, { 'p', 0xD0, 0, "Down" }, { 'p', 0xC8, 0, "Up (takes the phone out)" },
			{ 'w', 0, 2.5f, nullptr }, { 'l', 0, 0, "phone out (home screen)" },
			{ 't', 0x1C, 0, "Enter" }, { 'w', 0, 2.0f, nullptr }, { 'l', 0, 0, "menu" },
			{ 't', 0x1C, 0, "Enter" }, { 'w', 0, 2.5f, nullptr }, { 'l', 0, 0, "phone book" },
			{ 't', 0xD0, 0, "Down" }, { 'w', 0, 0.6f, nullptr }, { 't', 0xD0, 0, "Down" }, { 'w', 0, 2.0f, nullptr }, { 'l', 0, 0, "phone book, two down" },
			{ 't', 0x0E, 0, "Backspace" }, { 'w', 0, 1.5f, nullptr }, { 't', 0x0E, 0, "Backspace" }, { 'w', 0, 1.5f, nullptr },
			{ 'l', 0, 0, "after two Backspaces" }, { 't', 0x0E, 0, "Backspace" }, { 'w', 0, 2.5f, nullptr }, { 'l', 0, 0, "after three Backspaces" },
		};

		void PhoneTestTick(float a_dt)
		{
			auto& pt = phoneTest;
			if (!Config::Get().debugPhone || pt.step >= static_cast<int>(sizeof(kPhoneSteps) / sizeof(kPhoneSteps[0]))) {
				return;
			}
			pt.t += a_dt;
			if (pt.step < 0) {
				// (Niko mode too: the same keys in plain GTA IV, for comparison)
				const bool niko = Game::State().nikoMode.load(std::memory_order_relaxed) && !Game::State().gtaMenuOpen.load(std::memory_order_relaxed);
				if ((!captured && !niko) || !focused.load()) {
					pt.t = 0.0f;
				} else if (pt.t >= 15.0f) {
					pt.step = 0;
					pt.sub = 0;
					pt.t = 0.0f;
					LC_LOG("DebugPhone: starting (%s for 15 s)", captured ? "puppet mode" : "Niko mode");
				}
				return;
			}
			const auto& s = kPhoneSteps[pt.step];
			bool        next = false;
			switch (s.kind) {
			case 'p':
				// 0: baseline 0.15 s; 1: key down, hold 0.25 s while recording; 2: key up, 0.35 s gap
				if (pt.sub == 0) {
					pt.capture = true;
					if (pt.t >= 0.15f) {
						pt.base = pt.raw;
						pt.changed.fill(false);
						SendKey(s.key, true);
						pt.sub = 1;
						pt.t = 0.0f;
					}
				} else if (pt.sub == 1) {
					for (std::size_t i = 0; i < pt.raw.size(); ++i) {
						pt.changed[i] = pt.changed[i] || pt.raw[i] != pt.base[i];
					}
					if (pt.t >= 0.25f) {
						SendKey(s.key, false);
						pt.capture = false;
						char text[512];
						int  n = 0;
						for (std::size_t i = 0; i < pt.changed.size() && n < 480; ++i) {
							if (pt.changed[i]) {
								n += std::snprintf(text + n, sizeof(text) - n, " %u", static_cast<unsigned>(i));
							}
						}
						text[n] = 0;
						LC_LOG("DebugPhone: key %s feeds pad controls:%s", s.label, n ? text : " none");
						pt.sub = 2;
						pt.t = 0.0f;
					}
				} else if (pt.t >= 0.35f) {
					next = true;
				}
				break;
			case 't':
				if (pt.sub == 0) {
					SendKey(s.key, true);
					LC_LOG("DebugPhone: tap %s", s.label);
					pt.sub = 1;
					pt.t = 0.0f;
				} else if (pt.t >= 0.12f) {
					SendKey(s.key, false);
					next = true;
				}
				break;
			case 'w':
				next = pt.t >= s.seconds;
				break;
			default: {
				int ped = 0, sub = -1;
				::Scripting::GET_PLAYER_CHAR(static_cast<int>(::Scripting::GET_PLAYER_ID()), &ped);
				const bool task = ped && ::Scripting::GET_MOBILE_PHONE_TASK_SUB_TASK(ped, &sub);
				LC_LOG("DebugPhone: SCREENSHOT %s: phone %s, on screen %d, Niko's phone task %d sub %d", s.label, phoneOut.load() ? "out" : "away",
					static_cast<int>(::Scripting::CAN_PHONE_BE_SEEN_ON_SCREEN()), static_cast<int>(task), sub);
				next = true;
				break;
			}
			}
			if (next) {
				++pt.step;
				pt.sub = 0;
				pt.t = 0.0f;
				if (pt.step >= static_cast<int>(sizeof(kPhoneSteps) / sizeof(kPhoneSteps[0]))) {
					LC_LOG("DebugPhone: done");
				}
			}
		}

		void FocusTestTick(float a_dt)
		{
			auto& ft = focusTest;
			if (!Config::Get().debugFocusCycle || ft.cycle >= 2) {
				return;
			}
			ft.t += a_dt;
			const auto probe = [&](int a_next) {
				ft.rawAt = rawMouseTotal.load();
				InjectMouseMoves();
				ft.phase = a_next;
				ft.t = 0.0f;
			};
			switch (ft.phase) {
			case 0:  // puppeting for a while
				if (!captured || !focused.load()) {
					ft.t = 0.0f;
				} else if (ft.t >= (ft.cycle == 0 ? 15.0f : 20.0f)) {
					probe(1);
				}
				break;
			case 1:  // the probe before: then away
				if (ft.t >= 0.5f) {
					LogProbe("before the focus loss");
					watchdogOff = ft.cycle == 0;
					ft.thiefDone = false;
					if (HANDLE h = ::CreateThread(nullptr, 0, &FocusThief, nullptr, 0, nullptr)) {
						::CloseHandle(h);
						ft.phase = 2;
					} else {
						ft.cycle = 2;
					}
					ft.t = 0.0f;
				}
				break;
			case 2:  // away (the game loop mostly blocks meanwhile)
				if (ft.thiefDone.load()) {
					ft.phase = 3;
					ft.t = 0.0f;
				}
				break;
			case 3:  // back for 1 s: probe
				if (ft.t >= 1.0f) {
					probe(4);
				}
				break;
			case 4:
				if (ft.t >= 0.5f) {
					LogProbe("1 s after the focus came back");
					if (watchdogOff) {
						watchdogOff = false;
						LC_LOG("DebugFocusCycle 1: turning the raw mouse watchdog on");
						ft.phase = 5;
					} else {
						++ft.cycle;
						ft.phase = 0;
					}
					ft.t = 0.0f;
				}
				break;
			case 5:  // the first cycle: probe again with the watchdog on
				if (ft.t >= 1.0f) {
					probe(6);
				}
				break;
			default:
				if (ft.t >= 0.5f) {
					LogProbe("1 s after turning the watchdog on");
					++ft.cycle;
					ft.phase = 0;
					ft.t = 0.0f;
				}
				break;
			}
		}
	}

	void Install()
	{
		if (originalProc) {
			return;
		}
		HWND hwnd = rage::g_pHWND;
		if (!hwnd || !::IsWindow(hwnd)) {
			return;
		}
		window = hwnd;
		::SetLastError(0);
		const auto previousProc = ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc));
		if (!previousProc && ::GetLastError() != 0) {
			if (!installLogged) {
				installLogged = true;
				LC_LOG("ERROR: couldn't subclass the game window %p (error %lu); no keyboard/mouse for Minecraft", static_cast<void*>(hwnd), ::GetLastError());
			}
			window = nullptr;
			return;
		}
		originalProc = reinterpret_cast<WNDPROC>(previousProc);
		focused = ::GetForegroundWindow() == hwnd;
		LC_LOG("game window %p subclassed (focused %d); menu key dik 0x%02X", static_cast<void*>(hwnd), focused.load(), Config::Get().MenuKeyDik());
		LogRawInputRegistrations();
	}

	void Uninstall(bool a_quiet)
	{
		quiet = a_quiet;
		SetCapture(false);
		if (window && originalProc && ::IsWindow(window)) {
			if (reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(window, GWLP_WNDPROC)) == &WndProc) {
				::SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalProc));
			}
		}
		originalProc = nullptr;
		window = nullptr;
	}

	void SetCapture(bool a_capture)
	{
		if (a_capture == captured || (a_capture && !window)) {
			return;
		}
		if (a_capture) {
			// Remember the process's mouse registration (Wine's dinput or the game may own one),
			// take it for the game window, and give it back when puppet mode ends.
			hadPrevious = false;
			UINT count = 0;
			::GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
			if (count) {
				std::vector<RAWINPUTDEVICE> devices(count);
				if (::GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE)) != static_cast<UINT>(-1)) {
					for (const auto& d : devices) {
						if (d.usUsagePage == 0x01 && d.usUsage == 0x02) {
							previous = d;
							hadPrevious = true;
						}
					}
				}
			}
			RAWINPUTDEVICE mouse{ 0x01, 0x02, RIDEV_INPUTSINK, window };
			if (::RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) {
				captured = true;
				LC_LOG("raw mouse registered for the game window (previous registration: %s)", hadPrevious ? "saved" : "none");
			} else {
				LC_LOG("ERROR: RegisterRawInputDevices(mouse) failed (error %lu): no mouse look", ::GetLastError());
			}
		} else {
			RAWINPUTDEVICE mouse{ 0x01, 0x02, RIDEV_REMOVE, nullptr };
			if (hadPrevious) {
				mouse = previous;
			}
			const bool ok = ::RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) != FALSE;
			if (!quiet) {
				if (!ok) {
					LC_LOG("raw mouse: restoring the previous registration failed (error %lu)", ::GetLastError());
				} else {
					LC_LOG("raw mouse %s", hadPrevious ? "registration restored" : "unregistered");
				}
			}
			captured = false;
			lookDx = 0;
			lookDy = 0;
		}
	}

	void Pad(CPad* a_pad)
	{
		Perf::Scope timer(Perf::kPad);
		const auto& st = Game::State();
		if (!a_pad || a_pad != CPad::GetPad()) {
			return;  // only the local player's pad
		}
		HostDrive::Pad(a_pad);  // GTA's enter/exit controls while a vehicle action presses them
		if (!st.puppeting.load(std::memory_order_relaxed) || st.gtaMenuOpen.load(std::memory_order_relaxed)) {
			return;
		}
		// GTA reads keyboard and mouse through DirectInput into CPad, so swallowing window messages
		// isn't enough: clear the controls after the pad update. The pause control stays unless a
		// Minecraft screen is open (then Esc closes that screen instead).
		const bool keepPause = !st.mcScreenOpen.load(std::memory_order_relaxed);
		constexpr int kControls = static_cast<int>(sizeof(a_pad->m_aValues) / sizeof(a_pad->m_aValues[0]));
		if (phoneTest.capture) {
			for (int i = 0; i < kControls && i < static_cast<int>(phoneTest.raw.size()); ++i) {
				phoneTest.raw[i] = a_pad->m_aValues[i].m_nCurrentValue;
			}
		}
		// GTA's phone (PhoneKeys): its controls stay (no Minecraft screen open: then every key is Minecraft's).
		const bool phone = Config::Get().phoneKeys && !st.mcScreenOpen.load(std::memory_order_relaxed);
		const bool out = phoneOut.load(std::memory_order_relaxed);
		for (int i = 0; i < kControls; ++i) {
			if (keepPause && i == INPUT_FRONTEND_PAUSE) {
				continue;
			}
			if (phone && IsPhoneControl(i, out)) {
				continue;
			}
			a_pad->m_aValues[i].m_nCurrentValue = 0;
			a_pad->m_aValues[i].m_nLastValue = 0;
		}
		counters.padZeroed.fetch_add(1, std::memory_order_relaxed);
	}

	void Tick(float a_dt)
	{
		Watchdog(a_dt);
		FocusTestTick(a_dt);
		UpdatePhone();
		PhoneTestTick(a_dt);
	}

	bool PhoneOut()
	{
		return phoneOut.load(std::memory_order_relaxed);
	}

	void ConsumeLook(float& a_dx, float& a_dy)
	{
		a_dx = static_cast<float>(lookDx.exchange(0, std::memory_order_relaxed));
		a_dy = static_cast<float>(lookDy.exchange(0, std::memory_order_relaxed));
	}

	void ReleaseAll()
	{
		Push(proto::kInReleaseAll, 0);
		counters.releaseAll.fetch_add(1, std::memory_order_relaxed);
	}

	Counters TakeCounters()
	{
		auto take = [](std::atomic<std::uint32_t>& a_c) { return a_c.exchange(0, std::memory_order_relaxed); };
		return { take(counters.keys), take(counters.buttons), take(counters.scrolls), take(counters.chars), take(counters.cursors), take(counters.rawMouse),
			take(counters.releaseAll), take(counters.openMenu), take(counters.dropped), take(counters.padZeroed), take(counters.rawRetaken) };
	}
}

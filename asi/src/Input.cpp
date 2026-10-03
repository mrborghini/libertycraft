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

		struct AtomicCounters
		{
			std::atomic<std::uint32_t> keys{ 0 }, buttons{ 0 }, scrolls{ 0 }, chars{ 0 }, cursors{ 0 }, rawMouse{ 0 }, releaseAll{ 0 }, openMenu{ 0 },
				dropped{ 0 }, padZeroed{ 0 };
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
		for (int i = 0; i < static_cast<int>(sizeof(a_pad->m_aValues) / sizeof(a_pad->m_aValues[0])); ++i) {
			if (keepPause && i == INPUT_FRONTEND_PAUSE) {
				continue;
			}
			a_pad->m_aValues[i].m_nCurrentValue = 0;
			a_pad->m_aValues[i].m_nLastValue = 0;
		}
		counters.padZeroed.fetch_add(1, std::memory_order_relaxed);
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
			take(counters.releaseAll), take(counters.openMenu), take(counters.dropped), take(counters.padZeroed) };
	}
}

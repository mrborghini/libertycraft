// GTA's help text offering context actions and menu choices ("Press E to play a half game. Press Enter to
// play a full game.": the text names the controls as ~INPUT_PICKUP~, ~ACCEPT~, ...). Pure logic, no IV-SDK:
// Missions.cpp reads the text, Input lets those controls through while puppet mode zeroes the pad (and keeps
// their keys from Minecraft); asi/tests/scene_test.cpp tests it.
#pragma once

#include "drive/PadNames.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lc::drive
{
	// The menu-type choices a help text can offer. Attacks, aiming, moves and the phone, which hints ask for
	// too, are never let through while Minecraft drives.
	enum PromptAction : unsigned
	{
		kPromptPickup = 1u << 0,  // ~INPUT_PICKUP~ (E): use, play, buy, quit
		kPromptAccept = 1u << 1,  // ~INPUT_FRONTEND_ACCEPT~ or ~ACCEPT~ (Enter): a menu's other choice ("a full game")
		kPromptCancel = 1u << 2,  // ~INPUT_FRONTEND_CANCEL~ or ~CANCEL~ (Backspace): back
	};

	// The ~TOKEN~ name at a_text[a_at] (after a '~'), up to the next '~'; false if it isn't one.
	inline bool PromptToken(const std::uint16_t* a_text, std::size_t a_max, std::size_t a_at, char (&a_name)[48], std::size_t& a_end)
	{
		std::size_t n = 0;
		for (std::size_t j = a_at; j < a_max && a_text[j]; ++j) {
			const std::uint16_t c = a_text[j];
			if (c == '~') {
				a_name[n] = 0;
				a_end = j;
				return n > 0;
			}
			if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') || n >= sizeof(a_name) - 1) {
				return false;
			}
			a_name[n++] = static_cast<char>(c);
		}
		return false;  // cut off
	}

	// Every menu-type choice a help text (UTF-16, up to a_max characters or a 0) offers, as PromptAction bits.
	inline unsigned PromptActions(const std::uint16_t* a_text, std::size_t a_max)
	{
		unsigned bits = 0;
		if (!a_text) {
			return bits;
		}
		for (std::size_t i = 0; i < a_max && a_text[i]; ++i) {
			if (a_text[i] != '~') {
				continue;
			}
			char        name[48];
			std::size_t end = 0;
			if (!PromptToken(a_text, a_max, i + 1, name, end)) {
				continue;
			}
			if (std::strcmp(name, "INPUT_PICKUP") == 0) {
				bits |= kPromptPickup;
			} else if (std::strcmp(name, "INPUT_FRONTEND_ACCEPT") == 0 || std::strcmp(name, "ACCEPT") == 0) {
				bits |= kPromptAccept;
			} else if (std::strcmp(name, "INPUT_FRONTEND_CANCEL") == 0 || std::strcmp(name, "CANCEL") == 0) {
				bits |= kPromptCancel;
			}
			i = end;  // (the closing '~' isn't the next token's opening one)
		}
		return bits;
	}

	// The pad control each choice is (ePadControls order, PadNames.h), -1 if the names don't have it.
	inline int PromptControlOf(unsigned a_action)
	{
		const char* want = a_action == kPromptPickup ? "PICKUP" : a_action == kPromptAccept ? "FRONTEND_ACCEPT" : a_action == kPromptCancel ? "FRONTEND_CANCEL" : nullptr;
		for (int c = 0; want && c < pad::kControlCount; ++c) {
			if (std::strcmp(pad::kControlNames[c], want) == 0) {
				return c;
			}
		}
		return -1;
	}
}

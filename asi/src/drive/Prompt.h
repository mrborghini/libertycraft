// GTA's help text asking for a context action ("Press E to bowl": the text names the control as
// ~INPUT_PICKUP~). Pure logic, no IV-SDK: Missions.cpp reads the text, Input lets the control through
// while puppet mode zeroes the pad (and keeps the key from Minecraft); asi/tests/scene_test.cpp tests it.
#pragma once

#include "drive/PadNames.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lc::drive
{
	// The first ~INPUT_<NAME>~ token in a help text (UTF-16, up to a_max characters or a 0): its control
	// (ePadControls order, PadNames.h), or -1.
	inline int PromptControl(const std::uint16_t* a_text, std::size_t a_max)
	{
		if (!a_text) {
			return -1;
		}
		static constexpr char kPrefix[] = "INPUT_";
		constexpr std::size_t kPrefixLen = sizeof(kPrefix) - 1;
		for (std::size_t i = 0; i < a_max && a_text[i]; ++i) {
			std::size_t k = 0;
			while (k < kPrefixLen && i + k < a_max && a_text[i + k] == static_cast<std::uint16_t>(kPrefix[k])) {
				++k;
			}
			if (k != kPrefixLen) {
				continue;
			}
			char        name[48]{};
			std::size_t n = 0;
			for (std::size_t j = i + kPrefixLen; j < a_max && n < sizeof(name) - 1; ++j) {
				const std::uint16_t c = a_text[j];
				if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
					break;
				}
				name[n++] = static_cast<char>(c);
			}
			for (int c = 0; c < pad::kControlCount; ++c) {
				if (std::strcmp(name, pad::kControlNames[c]) == 0) {
					return c;
				}
			}
		}
		return -1;
	}

	// The context actions puppet mode lets through to GTA while a help text asks for one (and keeps the
	// prompt key from Minecraft): picking up / using (INPUT_PICKUP, E on PC's keyboard). Not attacks,
	// moves or the phone, which hints ask for too.
	inline bool IsContextAction(int a_control)
	{
		return a_control >= 0 && a_control < pad::kControlCount && std::strcmp(pad::kControlNames[a_control], "PICKUP") == 0;
	}
}

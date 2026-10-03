// LibertyCraft.ini, next to LibertyCraft.asi (<gamedir>/plugins/). Written with defaults when
// missing. A ~50-line INI reader: [sections] are accepted and ignored, keys are case-insensitive,
// `;` and `#` start comments.
#pragma once

#include <cstdint>
#include <string>

namespace lc
{
	struct Config
	{
		// Minecraft drives the player ped and the camera. 0: the plugin only watches and logs.
		bool puppet = true;
		// "final": overwrite CCamera::m_pFinalCam's matrix/FOV after the game's camera update
		// (processCameraEvent). "scripted": a CREATE_CAM(14) camera driven with natives.
		enum class CameraMode { kFinal, kScripted } cameraMode = CameraMode::kFinal;
		// How McState::fovDeg (Minecraft's vertical FOV) maps onto CCam::m_fFOV:
		// "vertical": GTA IV's FOV is vertical too (it's a Hor+ game, default on-foot FOV ~45):
		// copied as is. "horizontal43": treat m_fFOV as the horizontal FOV of a 4:3 view (SkyCraft's
		// Skyrim convention) in case that turns out to be what RAGE wants.
		enum class FovMode { kVertical, kHorizontal43 } fovMode = FovMode::kVertical;
		// Key that opens Minecraft's pause/options menu (Esc is GTA's). A letter, digit or F1-F12.
		std::string menuKey = "O";
		// Detailed per-second state lines and camera/axis discovery chatter.
		bool diagnostics = false;
		// One line a minute (10 s with Diagnostics) of where the frame time goes.
		bool logPerf = true;
		// FREEZE_CHAR_POSITION while puppeting (0: zero the velocity every frame instead).
		bool freezePed = true;
		// Metres from the ped's reported position (its root) down to its feet, used until the
		// plugin has measured it with GET_CHAR_HEIGHT_ABOVE_GROUND (0 = always use this value).
		float rootToFeet = 1.0f;
		bool  measureRootToFeet = true;
		// Collision heightfield probe start. "top": from kProbeTopZ (1000 m), so buildings come out
		// as solid columns up to their roofs (walls work; overpasses become pillars). "feet": from
		// probeHeight metres above the player's feet (you can walk under bridges, but buildings are
		// only as tall as the probe start).
		enum class ProbeFrom { kTop, kFeet } probeFrom = ProbeFrom::kTop;
		float probeHeight = 3.0f;
		// Camera matrix rows (which CMatrix row is the camera's right/forward/up), discovered at
		// runtime by default ("auto"); or e.g. "0,1,2" / "-0,1,2" (leading '-' flips a row's sign).
		std::string cameraRows = "auto";

		static Config& Get();
		// Loads <asi dir>/LibertyCraft.ini (creating it with defaults if absent). Logs what it got.
		void Load(const wchar_t* a_asiDir);
		// menuKey as a DirectInput (set 1) scancode, 0 if unknown.
		std::uint8_t MenuKeyDik() const;
	};
}

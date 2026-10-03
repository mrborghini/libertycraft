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

		// ---- Minecraft's blocks and HUD (Render, Overlay) ----
		// Draw Minecraft's world and HUD in GTA's frame (a draw command run by the render thread).
		// 0: only drain the render ring, draw nothing.
		bool render = true;
		// The blocks' camera: auto (the render phase's grcViewport, else the final camera), phase,
		// current (grcViewport::sm_pCurrent), finalcam (TheCamera.m_pFinalCam).
		enum class RenderCamera { kAuto, kPhase, kCurrent, kFinalCam } renderCamera = RenderCamera::kAuto;
		// The game's depth buffer: auto (logarithmic with FusionFix loaded, else standard), log,
		// standard, off (no occlusion by GTA's world).
		enum class RenderDepth { kAuto, kLog, kStandard, kOff } renderDepth = RenderDepth::kAuto;
		// Brightness multiplier for Minecraft's blocks.
		float renderExposure = 1.0f;
		// Minecraft's HUD/GUI: auto (while puppeting or a Minecraft screen is open), always (whenever
		// Minecraft is alive), off.
		enum class OverlayMode { kAuto, kAlways, kOff } overlay = OverlayMode::kAuto;

		// ---- combat (Combat.h) ----
		// Minecraft and GTA IV fight each other: the actor table, Minecraft's hits/explosions/death,
		// GTA damage to the puppeted player as Minecraft damage. 0: events are drained and ignored.
		bool combat = true;
		// Minecraft damage (half-hearts) x this = GTA health taken off a ped (ambient peds have 100).
		float pedDamageScale = 10.0f;
		// GTA damage to the puppeted player / this = Minecraft damage (20 = full health).
		float playerDamageScale = 10.0f;
		// ADD_EXPLOSION type for Minecraft's explosions (0 grenade, 2 rocket, ...) and radius scale.
		int   explosionType = 0;
		float explosionRadiusScale = 1.0f;
		// A Minecraft hit knocks the ped over (SWITCH_PED_TO_RAGDOLL + APPLY_FORCE_TO_PED).
		bool ragdollOnHit = true;
		// Test hooks (not in the default ini): DAMAGE_CHAR the puppeted player every 5 s; 15 s into a
		// session, move the player (out of a car / an interior) to the nearest street, among peds.
		bool combatSelfTest = false;
		bool debugWarpOutdoors = false;
		// ---- vehicles and Niko mode (HostDrive.h) ----
		// While Minecraft drives the player: hand Niko back to GTA IV and enter/steal the nearest
		// vehicle (GTA's own enter control). In the vehicle GTA drives (and its own F gets out).
		std::string vehicleKey = "F";
		// Minecraft mode <-> Niko mode (plain GTA IV, Minecraft's player just follows Niko).
		std::string toggleKey = "Backslash";
		// Hide Niko in vehicles in Minecraft mode (Minecraft's player sits there on its mount).
		bool hideNikoInVehicle = true;
		bool toggleStartsInMinecraft = true;
		// Metres from the ped's reported position (in a seat) down to the riding Minecraft player's
		// feet (Minecraft's rider sits ~0.6 above its feet, hips ~0.75).
		float vehicleSeatDrop = 0.75f;
		// The enter press didn't take within 2 s: "warp" (WARP_CHAR_INTO_CAR, the closest empty car
		// within 10 m), "task" (TASK_ENTER_CAR_AS_DRIVER, any car: walks there and pulls the driver
		// out; IV-SDK warns task natives may crash) or "none".
		std::string vehicleEnterFallback = "warp";
		// Test hooks (not in the default ini): toggle the mode every 10 s; press the vehicle key when
		// a car is within 12 m while puppeting (else park an empty test car next to Niko, after moving
		// him to the nearest road if he's indoors), and GTA's exit control after 12 s in one.
		bool debugAutoToggle = false;
		bool debugAutoVehicle = false;

		static Config& Get();
		// Loads <asi dir>/LibertyCraft.ini (creating it with defaults if absent). Logs what it got.
		void Load(const wchar_t* a_asiDir);
		// menuKey as a DirectInput (set 1) scancode, 0 if unknown.
		std::uint8_t MenuKeyDik() const;
		std::uint8_t VehicleKeyDik() const { return KeyDik(vehicleKey); }
		std::uint8_t ToggleKeyDik() const { return KeyDik(toggleKey); }
		// A key name -> DirectInput (set 1) scancode, 0 if unknown: a letter, digit, F1-F12, a name
		// (Backslash, Grave, Tab, Minus, Equals, LBracket, RBracket, Semicolon, Apostrophe, Comma,
		// Period, Slash, Space, Insert, Delete, Home, End, PageUp, PageDown, Numpad0-9, ...) or hex (0x2B).
		static std::uint8_t KeyDik(const std::string& a_name);
	};
}

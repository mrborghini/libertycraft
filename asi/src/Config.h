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
		// While puppeting, the player keeps GTA's player control (its pad input is zeroed anyway):
		// without it GTA's peds and cops take the player for a cutscene player and stop fighting or
		// chasing him (Combat.h, GtaCrimes). Not in the default ini: the puppeted ped keeps its
		// collision too (not needed for that: their shots and blows reach him either way).
		bool puppetPlayerControl = true;
		// Keep GTA IV's own HUD and radar while Minecraft drives the player (both HUDs at once);
		// GTA still hides them itself where it normally does (cutscenes, menus, mission scripts).
		bool gtaHud = true;
		bool puppetCollision = false;
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
		// Camera matrix rows (which CMatrix row is the camera's right/forward/up): "auto" pins the
		// known-good 0,1,2 and has the game's camera cross-check them, "discover" adopts what the
		// game's camera says; or e.g. "0,1,2" / "-0,1,2" (leading '-' flips a row's sign).
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
		// Light the blocks with GTA IV's sun/moon, ambient and fog (gta), or Minecraft's own light
		// levels (minecraft). render/Lighting.h.
		enum class RenderLighting { kGta, kMinecraft } renderLighting = RenderLighting::kGta;
		// Colour saturation of the GTA-lit blocks while GTA's tone mapping constants are unavailable
		// (otherwise GTA's own, from the timecycle).
		float renderSaturation = 0.8f;
		// Calibration (not in the default ini): the auto-exposure model's key and floor (Lighting.h).
		float renderExposureKey = 0.85f;
		float renderExposureFloor = 11.0f;
		// Test hooks (not in the default ini): pin GTA's clock to these hours in turn (e.g.
		// "12,19.5,23") and force these weather types (-1 leaves it; 0 extrasunny .. 4 rain .. 7
		// lightning), one step every debugStepSeconds; debugLightingAB alternates Minecraft's and GTA's
		// lighting within each step; debugLighting logs GTA's lighting constants.
		std::string debugTimeOfDay;
		std::string debugWeather;
		float       debugStepSeconds = 20.0f;
		bool        debugLightingAB = false;
		bool        debugLighting = false;
		// Test hook (not in the default ini): in a vehicle, SET_CAR_FORWARD_SPEED to this (m/s) for
		// 4 s of every 8 (Render.cpp), to see the rider stay in the seat at speed.
		float       debugVehicleSpeed = 0.0f;
		// Test hook: in a vehicle, draw the rider where Minecraft reports it (uncorrected) for 4 s of
		// every 8, logging each switch.
		bool        debugSeatAB = false;

		// ---- sun shadows (render/Shadows.h, render/ShadowPass.*) ----
		// The blocks take GTA IV's sun shadows (its cascade atlas) and cast their own, on themselves and
		// on GTA's world. 0: no shadows.
		bool  renderShadows = true;
		// Not in the default ini: the blocks cast shadows (0: they only take GTA's).
		bool  renderShadowCast = true;
		// How dark GTA's world gets in the blocks' shadow (1: as GTA shades its own shadows).
		float renderShadowStrength = 1.0f;
		// Metres the blocks' shadow-casting surfaces move away from the sun (against self-shadowing acne).
		float renderShadowBias = 0.05f;
		// Blocks farther than this (m) from the camera cast no shadows.
		float renderShadowDistance = 128.0f;
		// Test hooks (not in the default ini): log GTA's shadow constants (on changes and every 10 s);
		// switch the shadows off and on every N s (DebugShadowsAB=N), logging each switch.
		bool  debugShadows = false;
		float debugShadowsAB = 0.0f;
		// Test hook (not in the default ini): the blocks show their sun shadow term instead of their
		// colour (red GTA's, green GTA's and the blocks', blue 0.5).
		bool  debugShadowView = false;
		// Test hook (not in the default ini): "x,y,z,heading" (GTA): 12 s after the blocks are first
		// drawn, put the player there once (a sunny spot for shadow tests).
		std::string debugShadowSpot;

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
		// Minecraft damage x this = GTA body and engine health off a vehicle (1000 each; the engine burns
		// below 0, and a car with no body health left blows up).
		float vehicleDamageScale = 15.0f;
		// Minecraft's blocks are solid for GTA IV's peds and vehicles (NpcBlocks.h). Not in the default
		// ini: how a ped is moved back out of blocks (0 the entity's own SetPosition, 1
		// SET_CHAR_COORDINATES_NO_OFFSET).
		bool npcBlocks = true;
		int  npcPushMethod = 0;
		// Test hook (not in the default ini): -1 off; else Minecraft's hits shove peds a different way
		// each hit (0 world direction, 1 world direction in the ped's own frame, 2 the old flags, 3 no
		// force), cycling from this one, and log how far and which way each went.
		int debugKnockbackVariant = -1;
		// Test hook (not in the default ini): N s into play, park a car with people in it 4 m east of the
		// player (Combat.cpp TestCarHook); 0 off.
		int debugTestCar = 0;
		std::string debugTestCarModel = "admiral";  // its model (e.g. sabre: two doors, pcj: a motorbike)
		bool debugCarCover = false;  // DebugTestCar parks it empty, front axle beside the player, a ped in cover beyond its bonnet
		int debugBulletWall = 0;  // N s into play, a ped beside the player shoots across the blocks ahead of him (NpcBlocks)
		// Minecraft's attacks are crimes in GTA IV: victims fight back or flee, police seeing it (or
		// any witness of a killing) give the player a wanted level, hurting a cop always does.
		bool gtaCrimes = true;
		// Test hook (not in the default ini): N s into play, give the player 2 wanted stars and log the
		// police's interest (Combat.cpp); 0 off.
		int debugWanted = 0;
		// Test hook (not in the default ini): occupants Minecraft kills alternately die the old way
		// (no SET_CHAR_FORCE_DIE_IN_CAR), to compare where their bodies end up.
		bool debugDieInCarAB = false;
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
		// How the puppeted ped is placed every frame: "direct" (the SET_CHAR_COORDINATES natives' own
		// move, without their clearing of the destination, which deleted the cars and pedestrians the
		// player walked into) or "native" (SET_CHAR_COORDINATES_NO_OFFSET, the old way). Game.cpp.
		std::string puppetMove = "direct";
		// Test hooks (not in the default ini): walk the puppet target through a parked car and a
		// pedestrian with each move method and log whether they survive; give DebugAutoVehicle's test
		// car a driver (a carjack); take the window focus away and back twice (alt-tab, no keyboard).
		bool debugWalkThroughCar = false;
		bool debugVehicleDriver = false;
		bool debugFocusCycle = false;
		// Test hook: the vehicle key's taps of GTA's enter control as real key events (SendInput).
		bool debugInjectEnterKey = false;
		// GTA's phone in Minecraft mode: the arrow keys are GTA's (Up takes the phone out or answers
		// a call, the arrows navigate), and while the phone is out also Enter, Backspace and the
		// number keys; Minecraft doesn't get them. Input.cpp.
		bool phoneKeys = true;
		// Test hook (not in the default ini): real key events that take the phone out, open the
		// contacts, scroll and put it away again, and a log of which pad controls each key feeds.
		bool debugPhone = false;
		// A car running into the puppeted player (or one of GTA's explosions hurting them) knocks
		// them over: GTA ragdolls Niko along the hit and keeps him until he is back on his feet,
		// the hit hurts the Minecraft player. HostDrive.cpp.
		bool ragdollOnVehicleHit = true;
		// Test hooks (not in the default ini): DebugAutoVehicle's car drives off at speed and Niko
		// bails out of it; a test car is driven into the puppeted player every 40 s.
		bool debugBailOut = false;
		bool debugRunOver = false;
		// Test hook (not in the default ini): play this GTA IV cutscene (e.g. rom2_a) 20 s into puppet mode.
		std::string debugCutscene;
		// Test hook (not in the default ini): give Niko this GTA weapon (7 pistol) 10 s into play.
		int debugGiveWeapon = 0;
		// Minecraft's fire, lava and magma burn GTA's peds on foot (and lava sets vehicles alight). Hazards.h.
		bool hazardsBurnPeds = true;
		bool hazardsBurnVehicles = true;   // fire, lava and magma under a vehicle: engine fire, lava wrecks it
		bool liquidsSlowVehicles = true;       // Minecraft water and lava slow vehicles (drag by depth; deep water stalls after a few s)
		bool liquidsSlowPeds = true;           // peds wade slower in Minecraft water, the deeper the slower
		// Experimental (not in the default ini): GTA's water level query (0x9AB6C0) answers Minecraft water. Measured: GTA's peds
		// (swimming) and vehicle buoyancy don't use it, so it changes little; off.
		bool minecraftWaterIsGtaWater = false;
		// Test hooks (not in the default ini): to the street 15 s into puppet mode; "fire"/"lava": a
		// 3 x 3 patch of it (in the plugin only) under the nearest walking ped every 20 s; "harbour": a
		// test car with Niko in it dropped into GTA's own water, its engine timed (Hazards.cpp DebugHarbour).
		bool        debugHazards = false;
		float       debugDriveThrottle = 0.0f;  // test hook: in a car, hold GTA's accelerator this many seconds (from 1 s in)
		std::string debugHazardInject;
		// ---- the Minecraft body on Niko's skeleton (Body.h, render/Body.h) ----
		// While GTA IV animates Niko itself, show the player's Minecraft body following his
		// animation (Niko hidden): master switch, in cutscenes, getting into / driving / getting out
		// of vehicles (else the Minecraft player rides its mount at the seat), and in Niko mode.
		bool minecraftBody = true;
		bool minecraftBodyCutscenes = true;
		bool minecraftBodyVehicles = true;
		bool minecraftBodyNikoMode = false;
		// The body's size on top of the automatic fit to Niko (1: torso and arms his size, the top of
		// the head at his, the feet on the ground); larger covers more of him.
		float minecraftBodyScale = 1.0f;
		// Not in the default ini: how Niko is hidden under the body ("visible": SET_CHAR_VISIBLE,
		// "alpha": SET_PED_ALPHA 0, in case an invisible ped stopped animating); log the bones
		// (DebugBody: once a second, plus a ped scan in cutscenes).
		std::string minecraftBodyHide = "visible";
		bool        debugBody = false;

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

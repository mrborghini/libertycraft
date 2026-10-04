#define LC_MODULE "config"
#include "Config.h"

#include "Log.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace lc
{
	namespace
	{
		constexpr const char* kDefaultIni =
			"; LibertyCraft.ini: see asi/README.md\n"
			"[LibertyCraft]\n"
			"; Minecraft drives Niko and the camera (0: only watch and log)\n"
			"Puppet=1\n"
			"; final | scripted\n"
			"CameraMode=final\n"
			"; vertical | horizontal43: how Minecraft's vertical FOV maps onto the game camera\n"
			"FovMode=vertical\n"
			"; key that opens Minecraft's pause/options menu (a letter, digit or F1-F12)\n"
			"MenuKey=O\n"
			"; per-second state lines, camera axis discovery chatter\n"
			"Diagnostics=0\n"
			"; one line a minute of where the frame time goes\n"
			"LogPerf=1\n"
			"; FREEZE_CHAR_POSITION while puppeting (0: zero the ped's velocity every frame instead)\n"
			"FreezePed=1\n"
			"; metres from the ped's root position down to its feet; measured in-game when MeasureRootToFeet=1\n"
			"RootToFeet=1.0\n"
			"MeasureRootToFeet=1\n"
			"; collision heightfield probe: top (from 1000 m: buildings are solid to the roof) | feet (from ProbeHeight m above the feet)\n"
			"ProbeFrom=top\n"
			"ProbeHeight=3.0\n"
			"; camera matrix rows right,forward,up: auto (0,1,2, checked against the game), discover, or e.g. 0,1,2 (a leading '-' flips a row)\n"
			"CameraRows=auto\n"
			"; draw Minecraft's blocks and HUD in GTA's frame (0: drain the render ring only)\n"
			"Render=1\n"
			"; blocks' camera: auto | phase | current | finalcam\n"
			"RenderCamera=auto\n"
			"; GTA's depth buffer: auto (log with FusionFix) | log | standard | off (blocks not hidden by GTA's world)\n"
			"RenderDepth=auto\n"
			"; GTA's glass (vehicle and shop windows) doesn't hide the blocks and your body behind it\n"
			"RenderBehindGlass=1\n"
			"; brightness multiplier for Minecraft's blocks\n"
			"RenderExposure=1.0\n"
			"; Minecraft's HUD: auto (puppeting or a Minecraft screen open) | always | off\n"
			"Overlay=auto\n"
			"; keep GTA IV's own HUD and radar next to Minecraft's in Minecraft mode (GTA still hides them in cutscenes and menus)\n"
			"GtaHud=1\n"
			"; blocks lit by GTA IV's sun, ambient and fog (gta) or Minecraft's own light levels (minecraft)\n"
			"RenderLighting=gta\n"
			"; colour saturation of the GTA-lit blocks when GTA's tone mapping can't be read (else GTA's own)\n"
			"RenderSaturation=0.8\n"
			"; sun shadows: the blocks take GTA IV's and cast their own, on themselves and on GTA's world\n"
			"RenderShadows=1\n"
			"; how dark GTA's world gets in the blocks' shadow (1: as GTA shades its own)\n"
			"RenderShadowStrength=1.0\n"
			"; combat: Minecraft hits/explosions/death reach GTA IV, GTA damage to the puppeted player reaches Minecraft\n"
			"Combat=1\n"
			"; Minecraft damage x this = GTA health off a ped; GTA damage to the player / this = Minecraft damage\n"
			"PedDamageScale=10\n"
			"PlayerDamageScale=10\n"
			"; ADD_EXPLOSION type for Minecraft explosions (0 grenade, 2 rocket, ...) and radius scale\n"
			"ExplosionType=0\n"
			"ExplosionRadiusScale=1.0\n"
			"; ADD_EXPLOSION type for a firework rocket with stars (2 rocket: the crossbow is an RPG)\n"
			"FireworkExplosionType=2\n"
			"; a Minecraft hit knocks the ped over (ragdoll)\n"
			"RagdollOnHit=1\n"
			"; how hard a Minecraft hit throws a ped or a body: 1 = a sword hit sends a ped 7 to 9 m, 0.25 about a gunshot's push\n"
			"HitForce=1.0\n"
			"; Minecraft damage x this = GTA body/engine health off a vehicle (1000 each)\n"
			"VehicleDamageScale=15\n"
			"; Minecraft blocks are solid for GTA IV's peds and vehicles\n"
			"NpcBlocks=1\n"
			"; Minecraft attacks are crimes: victims fight back or flee, witnesses and police give you a wanted level\n"
			"GtaCrimes=1\n"
			"; GTA's peds fight back against Minecraft's hostile mobs: armed peds and police shoot, the rest run; GTA's bullets hurt mobs\n"
			"PedsFightMobs=1\n"
			"; vehicles: this key (taken from Minecraft) enters/steals the nearest vehicle the GTA way; GTA's own F gets out\n"
			"VehicleKey=F\n"
			"; switches between Minecraft mode and Niko mode (plain GTA IV)\n"
			"ToggleKey=Backslash\n"
			"; hide Niko in vehicles in Minecraft mode (the Minecraft player sits there on its mount)\n"
			"HideNikoInVehicle=1\n"
			"ToggleStartsInMinecraft=1\n"
			"; GTA's phone in Minecraft mode: the arrow keys (and Enter, Backspace, numbers while it's out) go to GTA\n"
			"PhoneKeys=1\n"
			"; metres from the seated ped's position down to the riding Minecraft player's feet\n"
			"VehicleSeatDrop=0.75\n"
			"; if GTA's enter press didn't take: warp | task | none\n"
			"VehicleEnterFallback=warp\n"
			"; a car running into you (or a GTA explosion) knocks you over until Niko gets back up\n"
			"RagdollOnVehicleHit=1\n"
			"; while GTA IV animates Niko, show your Minecraft body following his animation instead of him\n"
			"MinecraftBody=1\n"
			"; ... in cutscenes, getting into / driving / getting out of vehicles, and in Niko mode\n"
			"MinecraftBodyCutscenes=1\n"
			"MinecraftBodyVehicles=1\n"
			"MinecraftBodyNikoMode=0\n"
			"; the body's size on top of the automatic fit to Niko (1.0)\n"
			"MinecraftBodyScale=1.0\n"
			"; Minecraft's fire, lava and magma burn GTA IV's peds, and vehicles (engine fire; lava wrecks them)\n"
			"HazardsBurnPeds=1\n"
			"HazardsBurnVehicles=1\n"
			"; vehicles struggle in Minecraft water and lava (slower the deeper; deep water stalls the engine)\n"
			"LiquidsSlowVehicles=1\n"
			"; peds wade slower in Minecraft water\n"
			"LiquidsSlowPeds=1\n"
			"; a mission script that takes the player over on foot (its camera, his control off, its tasks, a minigame) gets him until it lets go\n"
			"ScriptScenes=1\n"
			"; Minecraft's mobs leave mission characters alone (no hunting them, no mob's hit or blast reaches them)\n"
			"MissionPedsSafe=1\n"
			"; GTA's cutscenes and the scripts' cameras pause Minecraft (its mobs and sounds)\n"
			"ScenesPauseMinecraft=1\n";

		std::string Lower(std::string a_s)
		{
			std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}

		std::string Trim(const std::string& a_s)
		{
			const auto b = a_s.find_first_not_of(" \t\r\n");
			if (b == std::string::npos) {
				return {};
			}
			const auto e = a_s.find_last_not_of(" \t\r\n");
			return a_s.substr(b, e - b + 1);
		}

		// key (lower-case) -> value, sections flattened.
		std::unordered_map<std::string, std::string> Parse(const std::string& a_text)
		{
			std::unordered_map<std::string, std::string> out;
			std::size_t                                  pos = 0;
			while (pos < a_text.size()) {
				auto end = a_text.find('\n', pos);
				if (end == std::string::npos) {
					end = a_text.size();
				}
				std::string line = a_text.substr(pos, end - pos);
				pos = end + 1;
				const auto comment = line.find_first_of(";#");
				if (comment != std::string::npos) {
					line.erase(comment);
				}
				line = Trim(line);
				if (line.empty() || line.front() == '[') {
					continue;
				}
				const auto eq = line.find('=');
				if (eq == std::string::npos) {
					continue;
				}
				out[Lower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
			}
			return out;
		}

		bool ToBool(const std::string& a_v, bool a_default)
		{
			const auto v = Lower(a_v);
			if (v == "1" || v == "true" || v == "yes" || v == "on") {
				return true;
			}
			if (v == "0" || v == "false" || v == "no" || v == "off") {
				return false;
			}
			return a_default;
		}
	}

	Config& Config::Get()
	{
		static Config config;
		return config;
	}

	void Config::Load(const wchar_t* a_asiDir)
	{
		std::wstring path = a_asiDir ? a_asiDir : L"";
		path += L"LibertyCraft.ini";

		std::string text;
		if (FILE* f = ::_wfopen(path.c_str(), L"rb")) {
			char buf[4096];
			std::size_t n;
			while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
				text.append(buf, n);
			}
			std::fclose(f);
		} else if (FILE* w = ::_wfopen(path.c_str(), L"wb")) {
			std::fputs(kDefaultIni, w);
			std::fclose(w);
			text = kDefaultIni;
			LC_LOG("wrote default LibertyCraft.ini");
		} else {
			LC_LOG("no LibertyCraft.ini and couldn't create one (error %lu); using defaults", ::GetLastError());
		}

		const auto kv = Parse(text);
		auto get = [&](const char* a_key) -> const std::string* {
			const auto it = kv.find(a_key);
			return it == kv.end() ? nullptr : &it->second;
		};
		if (auto v = get("puppet")) puppet = ToBool(*v, puppet);
		if (auto v = get("cameramode")) cameraMode = Lower(*v) == "scripted" ? CameraMode::kScripted : CameraMode::kFinal;
		if (auto v = get("fovmode")) fovMode = Lower(*v) == "horizontal43" ? FovMode::kHorizontal43 : FovMode::kVertical;
		if (auto v = get("menukey")) menuKey = *v;
		if (auto v = get("diagnostics")) diagnostics = ToBool(*v, diagnostics);
		if (auto v = get("logperf")) logPerf = ToBool(*v, logPerf);
		if (auto v = get("freezeped")) freezePed = ToBool(*v, freezePed);
		if (auto v = get("roottofeet")) rootToFeet = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("measureroottofeet")) measureRootToFeet = ToBool(*v, measureRootToFeet);
		if (auto v = get("probefrom")) probeFrom = Lower(*v) == "feet" ? ProbeFrom::kFeet : ProbeFrom::kTop;
		if (auto v = get("probeheight")) probeHeight = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("camerarows")) cameraRows = Lower(*v);
		if (auto v = get("render")) render = ToBool(*v, render);
		if (auto v = get("rendercamera")) {
			const auto c = Lower(*v);
			renderCamera = c == "phase" ? RenderCamera::kPhase : c == "current" ? RenderCamera::kCurrent : c == "finalcam" ? RenderCamera::kFinalCam : RenderCamera::kAuto;
		}
		if (auto v = get("renderdepth")) {
			const auto d = Lower(*v);
			renderDepth = d == "log" ? RenderDepth::kLog : d == "standard" ? RenderDepth::kStandard : d == "off" ? RenderDepth::kOff : RenderDepth::kAuto;
		}
		if (auto v = get("renderbehindglass")) renderBehindGlass = ToBool(*v, renderBehindGlass);
		if (auto v = get("debugbehindglassab")) debugBehindGlassAB = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("renderexposure")) renderExposure = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("overlay")) {
			const auto o = Lower(*v);
			overlay = o == "always" ? OverlayMode::kAlways : o == "off" ? OverlayMode::kOff : OverlayMode::kAuto;
		}
		if (auto v = get("renderlighting")) renderLighting = Lower(*v) == "minecraft" ? RenderLighting::kMinecraft : RenderLighting::kGta;
		if (auto v = get("rendersaturation")) renderSaturation = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("renderexposurekey")) renderExposureKey = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("renderexposurefloor")) renderExposureFloor = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("debugtimeofday")) debugTimeOfDay = *v;
		if (auto v = get("debugweather")) debugWeather = *v;
		if (auto v = get("debugstepseconds")) debugStepSeconds = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("debuglightingab")) debugLightingAB = ToBool(*v, debugLightingAB);
		if (auto v = get("debuglighting")) debugLighting = ToBool(*v, debugLighting);
		if (auto v = get("debugvehiclespeed")) debugVehicleSpeed = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("debugseatab")) debugSeatAB = ToBool(*v, debugSeatAB);
		if (auto v = get("rendershadows")) renderShadows = ToBool(*v, renderShadows);
		if (auto v = get("rendershadowcast")) renderShadowCast = ToBool(*v, renderShadowCast);
		if (auto v = get("rendershadowstrength")) renderShadowStrength = std::clamp(static_cast<float>(std::atof(v->c_str())), 0.0f, 1.0f);
		if (auto v = get("rendershadowbias")) renderShadowBias = std::clamp(static_cast<float>(std::atof(v->c_str())), 0.0f, 2.0f);
		if (auto v = get("rendershadowdistance")) renderShadowDistance = std::clamp(static_cast<float>(std::atof(v->c_str())), 0.0f, 1000.0f);
		if (auto v = get("debugshadows")) debugShadows = ToBool(*v, debugShadows);
		if (auto v = get("debugshadowsab")) debugShadowsAB = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("debugshadowview")) debugShadowView = ToBool(*v, debugShadowView);
		if (auto v = get("debugshadowspot")) debugShadowSpot = *v;
		if (auto v = get("debugframetrace")) debugFrameTrace = *v;
		if (auto v = get("combat")) combat = ToBool(*v, combat);
		if (auto v = get("peddamagescale")) pedDamageScale = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("playerdamagescale")) playerDamageScale = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("explosiontype")) explosionType = std::atoi(v->c_str());
		if (auto v = get("explosionradiusscale")) explosionRadiusScale = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("fireworkexplosiontype")) fireworkExplosionType = std::atoi(v->c_str());
		if (auto v = get("ragdollonhit")) ragdollOnHit = ToBool(*v, ragdollOnHit);
		if (auto v = get("hitforce")) hitForce = std::clamp(static_cast<float>(std::atof(v->c_str())), 0.0f, 5.0f);
		if (auto v = get("combatselftest")) combatSelfTest = ToBool(*v, combatSelfTest);
		if (auto v = get("debugwarpoutdoors")) debugWarpOutdoors = ToBool(*v, debugWarpOutdoors);
		if (auto v = get("vehicledamagescale")) vehicleDamageScale = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("npcblocks")) npcBlocks = ToBool(*v, npcBlocks);
		if (auto v = get("npcpushmethod")) npcPushMethod = std::atoi(v->c_str());
		if (auto v = get("debugknockbackvariant")) debugKnockbackVariant = std::atoi(v->c_str());
		if (auto v = get("debugtestcar")) debugTestCar = std::atoi(v->c_str());
		if (auto v = get("debugtestcarmodel")) debugTestCarModel = *v;
		if (auto v = get("debugcarcover")) debugCarCover = ToBool(*v, debugCarCover);
		if (auto v = get("debugbulletwall")) debugBulletWall = std::atoi(v->c_str());
		if (auto v = get("debugbumpped")) debugBumpPed = std::atoi(v->c_str());
		if (auto v = get("pedsfightmobs")) pedsFightMobs = ToBool(*v, pedsFightMobs);
		if (auto v = get("debugmobshoot")) debugMobShoot = std::atoi(v->c_str());
		if (auto v = get("debugmobfight")) debugMobFight = std::atoi(v->c_str());
		if (auto v = get("debugfireworktargets")) debugFireworkTargets = std::atoi(v->c_str());
		if (auto v = get("debugfireworkheliahead")) debugFireworkHeliAhead = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("debugstumblekind")) debugStumbleKind = std::atoi(v->c_str());
		if (auto v = get("gtacrimes")) gtaCrimes = ToBool(*v, gtaCrimes);
		if (auto v = get("debugwanted")) debugWanted = std::atoi(v->c_str());
		if (auto v = get("debugdieincarab")) debugDieInCarAB = ToBool(*v, debugDieInCarAB);
		if (auto v = get("debugseatedhurt")) debugSeatedHurt = Lower(*v);
		if (auto v = get("puppetplayercontrol")) puppetPlayerControl = ToBool(*v, puppetPlayerControl);
		if (auto v = get("gtahud")) gtaHud = ToBool(*v, gtaHud);
		if (auto v = get("puppetcollision")) puppetCollision = ToBool(*v, puppetCollision);
		if (auto v = get("vehiclekey")) vehicleKey = *v;
		if (auto v = get("togglekey")) toggleKey = *v;
		if (auto v = get("hidenikoinvehicle")) hideNikoInVehicle = ToBool(*v, hideNikoInVehicle);
		if (auto v = get("togglestartsinminecraft")) toggleStartsInMinecraft = ToBool(*v, toggleStartsInMinecraft);
		if (auto v = get("vehicleseatdrop")) vehicleSeatDrop = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("vehicleenterfallback")) vehicleEnterFallback = Lower(*v);
		if (auto v = get("debugautotoggle")) debugAutoToggle = ToBool(*v, debugAutoToggle);
		if (auto v = get("debugautovehicle")) debugAutoVehicle = ToBool(*v, debugAutoVehicle);
		if (auto v = get("puppetmove")) puppetMove = Lower(*v) == "native" ? "native" : "direct";
		if (auto v = get("debugwalkthroughcar")) debugWalkThroughCar = ToBool(*v, debugWalkThroughCar);
		if (auto v = get("debugvehicledriver")) debugVehicleDriver = ToBool(*v, debugVehicleDriver);
		if (auto v = get("debugfocuscycle")) debugFocusCycle = ToBool(*v, debugFocusCycle);
		if (auto v = get("debuginjectenterkey")) debugInjectEnterKey = ToBool(*v, debugInjectEnterKey);
		if (auto v = get("phonekeys")) phoneKeys = ToBool(*v, phoneKeys);
		if (auto v = get("debugphone")) debugPhone = ToBool(*v, debugPhone);
		if (auto v = get("debuginputscript")) debugInputScript = *v;
		if (auto v = get("ragdollonvehiclehit")) ragdollOnVehicleHit = ToBool(*v, ragdollOnVehicleHit);
		if (auto v = get("debugbailout")) debugBailOut = ToBool(*v, debugBailOut);
		if (auto v = get("debugrunover")) debugRunOver = ToBool(*v, debugRunOver);
		if (auto v = get("debugvehiclehit")) debugVehicleHit = Lower(*v);
		if (auto v = get("debugcutscene")) debugCutscene = *v;
		if (auto v = get("debuggiveweapon")) debugGiveWeapon = std::atoi(v->c_str());
		if (auto v = get("debugviewportroom")) debugViewportRoom = std::atoi(v->c_str());
		if (auto v = get("hazardsburnpeds")) hazardsBurnPeds = ToBool(*v, hazardsBurnPeds);
		if (auto v = get("hazardsburnvehicles")) hazardsBurnVehicles = ToBool(*v, hazardsBurnVehicles);
		if (auto v = get("liquidsslowvehicles")) liquidsSlowVehicles = ToBool(*v, liquidsSlowVehicles);
		if (auto v = get("minecraftwaterisgtawater")) minecraftWaterIsGtaWater = ToBool(*v, minecraftWaterIsGtaWater);
		if (auto v = get("liquidsslowpeds")) liquidsSlowPeds = ToBool(*v, liquidsSlowPeds);
		if (auto v = get("debughazards")) debugHazards = ToBool(*v, debugHazards);
		if (auto v = get("debugdrivethrottle")) debugDriveThrottle = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("debugpausemenu")) debugPauseMenu = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("debugpausemenucycles")) debugPauseMenuCycles = std::atoi(v->c_str());
		if (auto v = get("debugdrivewander")) debugDriveWander = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("debughazardinject")) debugHazardInject = Lower(*v);
		if (auto v = get("citymaterials")) cityMaterials = ToBool(*v, cityMaterials);
		if (auto v = get("debugmaterials")) debugMaterials = std::atoi(v->c_str());
		if (auto v = get("scriptscenes")) scriptScenes = ToBool(*v, scriptScenes);
		if (auto v = get("missionpedssafe")) missionPedsSafe = ToBool(*v, missionPedsSafe);
		if (auto v = get("scenespauseminecraft")) scenesPauseMinecraft = ToBool(*v, scenesPauseMinecraft);
		if (auto v = get("debugmissionprobe")) debugMissionProbe = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("debugmissionprobeab")) debugMissionProbeAB = ToBool(*v, debugMissionProbeAB);
		if (auto v = get("debugmissionblips")) debugMissionBlips = ToBool(*v, debugMissionBlips);
		if (auto v = get("debugmissionwarp")) debugMissionWarp = Lower(*v);
		if (auto v = get("minecraftbody")) minecraftBody = ToBool(*v, minecraftBody);
		if (auto v = get("minecraftbodycutscenes")) minecraftBodyCutscenes = ToBool(*v, minecraftBodyCutscenes);
		if (auto v = get("minecraftbodyvehicles")) minecraftBodyVehicles = ToBool(*v, minecraftBodyVehicles);
		if (auto v = get("minecraftbodynikomode")) minecraftBodyNikoMode = ToBool(*v, minecraftBodyNikoMode);
		if (auto v = get("minecraftbodyhide")) minecraftBodyHide = Lower(*v) == "alpha" ? "alpha" : "visible";
		if (auto v = get("minecraftbodyscale")) minecraftBodyScale = std::clamp(static_cast<float>(std::atof(v->c_str())), 0.5f, 2.0f);
		if (auto v = get("debugbody")) debugBody = ToBool(*v, debugBody);
		if (auto v = get("debugbodyview")) debugBodyView = *v;
		if (auto v = get("debugbodyviewseconds")) debugBodyViewSeconds = std::max(0.5f, static_cast<float>(std::atof(v->c_str())));
		if (auto v = get("debugbodyab")) debugBodyAB = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("debugtrainride")) debugTrainRide = static_cast<float>(std::atof(v->c_str()));

		LC_LOG("config: Puppet=%d CameraMode=%s FovMode=%s MenuKey=%s (dik 0x%02X) Diagnostics=%d LogPerf=%d FreezePed=%d RootToFeet=%.2f (measure %d) ProbeFrom=%s ProbeHeight=%.1f CameraRows=%s",
			puppet, cameraMode == CameraMode::kScripted ? "scripted" : "final", fovMode == FovMode::kHorizontal43 ? "horizontal43" : "vertical",
			menuKey.c_str(), MenuKeyDik(), diagnostics, logPerf, freezePed, rootToFeet, measureRootToFeet, probeFrom == ProbeFrom::kFeet ? "feet" : "top", probeHeight, cameraRows.c_str());
		static constexpr const char* kRenderCameras[] = { "auto", "phase", "current", "finalcam" };
		static constexpr const char* kRenderDepths[] = { "auto", "log", "standard", "off" };
		static constexpr const char* kOverlayModes[] = { "auto", "always", "off" };
		LC_LOG("config: Render=%d RenderCamera=%s RenderDepth=%s RenderExposure=%.2f Overlay=%s", render, kRenderCameras[static_cast<int>(renderCamera)],
			kRenderDepths[static_cast<int>(renderDepth)], renderExposure, kOverlayModes[static_cast<int>(overlay)]);
		LC_LOG("config: RenderLighting=%s RenderSaturation=%.2f (exposure key %.2f floor %.1f)%s%s%s%s", renderLighting == RenderLighting::kGta ? "gta" : "minecraft",
			renderSaturation, renderExposureKey, renderExposureFloor, debugTimeOfDay.empty() ? "" : (" DebugTimeOfDay=" + debugTimeOfDay).c_str(),
			debugWeather.empty() ? "" : (" DebugWeather=" + debugWeather).c_str(), debugLightingAB ? " DebugLightingAB=1" : "", debugLighting ? " DebugLighting=1" : "");
		LC_LOG("config: RenderShadows=%d RenderShadowStrength=%.2f RenderShadowBias=%.2f RenderShadowDistance=%.0f%s%s%s", renderShadows, renderShadowStrength,
			renderShadowBias, renderShadowDistance, renderShadowCast ? "" : " RenderShadowCast=0", debugShadows ? " DebugShadows=1" : "",
			debugShadowsAB > 0.0f ? (debugShadowView ? " DebugShadowsAB on DebugShadowView=1" : " DebugShadowsAB on") : (debugShadowView ? " DebugShadowView=1" : ""));
		if (debugVehicleSpeed > 0.0f) {
			LC_LOG("config: DebugVehicleSpeed=%.1f%s", debugVehicleSpeed, debugSeatAB ? " DebugSeatAB=1" : "");
		}
		LC_LOG("config: Combat=%d PedDamageScale=%.1f PlayerDamageScale=%.1f ExplosionType=%d ExplosionRadiusScale=%.2f FireworkExplosionType=%d RagdollOnHit=%d HitForce=%.2f%s%s",
			combat, pedDamageScale, playerDamageScale, explosionType, explosionRadiusScale, fireworkExplosionType, ragdollOnHit, hitForce,
			combatSelfTest ? " CombatSelfTest=1" : "", debugWarpOutdoors ? " DebugWarpOutdoors=1" : "");
		if (debugStumbleKind >= 0) {
			LC_LOG("config: DebugStumbleKind=%d", debugStumbleKind);
		}
		if (debugFireworkTargets > 0) {
			LC_LOG("config: DebugFireworkTargets=%d DebugFireworkHeliAhead=%.0f", debugFireworkTargets, debugFireworkHeliAhead);
		}
		LC_LOG("config: VehicleDamageScale=%.1f NpcBlocks=%d%s%s", vehicleDamageScale, npcBlocks, npcPushMethod ? " NpcPushMethod=1" : "",
			debugKnockbackVariant >= 0 ? " DebugKnockbackVariant on" : "");
		if (debugTestCar != 0) {
			LC_LOG("config: DebugTestCar=%d (%s)", debugTestCar, debugTestCarModel.c_str());
		}
		if (!debugSeatedHurt.empty()) {
			LC_LOG("config: DebugSeatedHurt=%s", debugSeatedHurt.c_str());
		}
		LC_LOG("config: GtaHud=%d GtaCrimes=%d PuppetPlayerControl=%d PuppetCollision=%d%s", gtaHud, gtaCrimes, puppetPlayerControl, puppetCollision,
			debugWanted > 0 ? " DebugWanted on" : "");
		LC_LOG("config: VehicleKey=%s (dik 0x%02X) ToggleKey=%s (dik 0x%02X) HideNikoInVehicle=%d ToggleStartsInMinecraft=%d VehicleSeatDrop=%.2f VehicleEnterFallback=%s%s%s",
			vehicleKey.c_str(), VehicleKeyDik(), toggleKey.c_str(), ToggleKeyDik(), hideNikoInVehicle, toggleStartsInMinecraft, vehicleSeatDrop,
			vehicleEnterFallback.c_str(), debugAutoToggle ? " DebugAutoToggle=1" : "", debugAutoVehicle ? " DebugAutoVehicle=1" : "");
		LC_LOG("config: PuppetMove=%s%s%s%s%s", puppetMove.c_str(), debugWalkThroughCar ? " DebugWalkThroughCar=1" : "",
			debugVehicleDriver ? " DebugVehicleDriver=1" : "", debugFocusCycle ? " DebugFocusCycle=1" : "", debugInjectEnterKey ? " DebugInjectEnterKey=1" : "");
		LC_LOG("config: PhoneKeys=%d%s", phoneKeys, debugPhone ? " DebugPhone=1" : "");
		LC_LOG("config: PedsFightMobs=%d DebugMobShoot=%d DebugMobFight=%d", pedsFightMobs, debugMobShoot, debugMobFight);
		LC_LOG("config: HazardsBurnPeds=%d HazardsBurnVehicles=%d LiquidsSlowVehicles=%d LiquidsSlowPeds=%d MinecraftWaterIsGtaWater=%d%s%s%s", hazardsBurnPeds,
			hazardsBurnVehicles, liquidsSlowVehicles, liquidsSlowPeds, minecraftWaterIsGtaWater,
			debugHazards ? " DebugHazards=1" : "", debugHazardInject.empty() ? "" : " DebugHazardInject=", debugHazardInject.c_str());
		LC_LOG("config: RagdollOnVehicleHit=%d%s%s%s%s%s%s", ragdollOnVehicleHit, debugBailOut ? " DebugBailOut=1" : "", debugRunOver ? " DebugRunOver=1" : "",
			debugCutscene.empty() ? "" : " DebugCutscene=", debugCutscene.c_str(), debugVehicleHit.empty() ? "" : " DebugVehicleHit=", debugVehicleHit.c_str());
		LC_LOG("config: MinecraftBody=%d MinecraftBodyCutscenes=%d MinecraftBodyVehicles=%d MinecraftBodyNikoMode=%d MinecraftBodyScale=%.2f MinecraftBodyHide=%s%s",
			minecraftBody, minecraftBodyCutscenes, minecraftBodyVehicles, minecraftBodyNikoMode, minecraftBodyScale, minecraftBodyHide.c_str(),
			debugBody ? " DebugBody=1" : "");
		LC_LOG("config: ScriptScenes=%d MissionPedsSafe=%d ScenesPauseMinecraft=%d%s%s%s", scriptScenes, missionPedsSafe, scenesPauseMinecraft,
			debugMissionBlips ? " DebugMissionBlips=1" : "", debugMissionWarp.empty() ? "" : " DebugMissionWarp=", debugMissionWarp.c_str());
		if (debugMissionProbe > 0.0f) {
			LC_LOG("config: DebugMissionProbe=%.0f%s", debugMissionProbe, debugMissionProbeAB ? " DebugMissionProbeAB=1" : "");
		}
		if (debugViewportRoom) {
			LC_LOG("config: DebugViewportRoom=%d", debugViewportRoom);
		}
		if (!debugBodyView.empty() || debugBodyAB > 0.0f || debugTrainRide > 0.0f) {
			LC_LOG("config: DebugBodyView=%s (%.1f s each) DebugBodyAB=%.1f DebugTrainRide=%.1f", debugBodyView.c_str(), debugBodyViewSeconds, debugBodyAB, debugTrainRide);
		}
	}

	std::uint8_t Config::MenuKeyDik() const
	{
		return KeyDik(menuKey);
	}

	std::uint8_t Config::KeyDik(const std::string& a_name)
	{
		const auto key = Lower(Trim(a_name));
		if (key.size() > 2 && key[0] == '0' && key[1] == 'x') {
			const long v = std::strtol(key.c_str() + 2, nullptr, 16);
			return v > 0 && v < 256 ? static_cast<std::uint8_t>(v) : 0;
		}
		static const std::unordered_map<std::string, std::uint8_t> kNames = {
			{ "backslash", 0x2B }, { "\\", 0x2B }, { "grave", 0x29 }, { "`", 0x29 }, { "tilde", 0x29 }, { "tab", 0x0F },
			{ "minus", 0x0C }, { "-", 0x0C }, { "equals", 0x0D }, { "=", 0x0D }, { "lbracket", 0x1A }, { "[", 0x1A },
			{ "rbracket", 0x1B }, { "]", 0x1B }, { "semicolon", 0x27 }, { ";", 0x27 }, { "apostrophe", 0x28 }, { "'", 0x28 },
			{ "comma", 0x33 }, { ",", 0x33 }, { "period", 0x34 }, { ".", 0x34 }, { "slash", 0x35 }, { "/", 0x35 },
			{ "space", 0x39 }, { "enter", 0x1C }, { "return", 0x1C }, { "backspace", 0x0E }, { "capslock", 0x3A },
			{ "insert", 0xD2 }, { "delete", 0xD3 }, { "home", 0xC7 }, { "end", 0xCF }, { "pageup", 0xC9 }, { "pagedown", 0xD1 },
			{ "up", 0xC8 }, { "down", 0xD0 }, { "left", 0xCB }, { "right", 0xCD }, { "lalt", 0x38 }, { "ralt", 0xB8 },
			{ "lctrl", 0x1D }, { "rctrl", 0x9D }, { "lshift", 0x2A }, { "rshift", 0x36 }, { "numpad0", 0x52 }, { "numpad1", 0x4F },
			{ "numpad2", 0x50 }, { "numpad3", 0x51 }, { "numpad4", 0x4B }, { "numpad5", 0x4C }, { "numpad6", 0x4D },
			{ "numpad7", 0x47 }, { "numpad8", 0x48 }, { "numpad9", 0x49 }, { "oem102", 0x56 },
		};
		if (const auto it = kNames.find(key); it != kNames.end()) {
			return it->second;
		}
		if (key.size() == 1) {
			static constexpr const char* kLetters = "abcdefghijklmnopqrstuvwxyz";
			static constexpr std::uint8_t kLetterDik[26] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
				0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };
			if (const char* p = std::strchr(kLetters, key[0]); p && key[0]) {
				return kLetterDik[p - kLetters];
			}
			if (key[0] >= '1' && key[0] <= '9') {
				return static_cast<std::uint8_t>(0x02 + (key[0] - '1'));
			}
			if (key[0] == '0') {
				return 0x0B;
			}
		}
		if (key.size() >= 2 && key[0] == 'f') {
			const int n = std::atoi(key.c_str() + 1);
			if (n >= 1 && n <= 10) {
				return static_cast<std::uint8_t>(0x3B + n - 1);
			}
			if (n == 11) {
				return 0x57;
			}
			if (n == 12) {
				return 0x58;
			}
		}
		return 0;
	}
}

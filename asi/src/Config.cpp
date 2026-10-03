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
			"; LibertyCraft.ini -- see asi/README.md\n"
			"[LibertyCraft]\n"
			"; Minecraft drives Niko and the camera (0: only watch and log)\n"
			"Puppet=1\n"
			"; final | scripted\n"
			"CameraMode=final\n"
			"; vertical | horizontal43 -- how Minecraft's vertical FOV maps onto the game camera\n"
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
			"; camera matrix rows right,forward,up: auto, or e.g. 0,1,2 (prefix - flips a row)\n"
			"CameraRows=auto\n"
			"; draw Minecraft's blocks and HUD in GTA's frame (0: drain the render ring only)\n"
			"Render=1\n"
			"; blocks' camera: auto | phase | current | finalcam\n"
			"RenderCamera=auto\n"
			"; GTA's depth buffer: auto (log with FusionFix) | log | standard | off (blocks not hidden by GTA's world)\n"
			"RenderDepth=auto\n"
			"; brightness multiplier for Minecraft's blocks\n"
			"RenderExposure=1.0\n"
			"; Minecraft's HUD: auto (puppeting or a Minecraft screen open) | always | off\n"
			"Overlay=auto\n"
			"; combat: Minecraft hits/explosions/death reach GTA IV, GTA damage to the puppeted player reaches Minecraft\n"
			"Combat=1\n"
			"; Minecraft damage x this = GTA health off a ped; GTA damage to the player / this = Minecraft damage\n"
			"PedDamageScale=10\n"
			"PlayerDamageScale=10\n"
			"; ADD_EXPLOSION type for Minecraft explosions (0 grenade, 2 rocket, ...) and radius scale\n"
			"ExplosionType=0\n"
			"ExplosionRadiusScale=1.0\n"
			"; a Minecraft hit knocks the ped over (ragdoll)\n"
			"RagdollOnHit=1\n";

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
		if (auto v = get("renderexposure")) renderExposure = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("overlay")) {
			const auto o = Lower(*v);
			overlay = o == "always" ? OverlayMode::kAlways : o == "off" ? OverlayMode::kOff : OverlayMode::kAuto;
		}
		if (auto v = get("combat")) combat = ToBool(*v, combat);
		if (auto v = get("peddamagescale")) pedDamageScale = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("playerdamagescale")) playerDamageScale = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("explosiontype")) explosionType = std::atoi(v->c_str());
		if (auto v = get("explosionradiusscale")) explosionRadiusScale = static_cast<float>(std::atof(v->c_str()));
		if (auto v = get("ragdollonhit")) ragdollOnHit = ToBool(*v, ragdollOnHit);
		if (auto v = get("combatselftest")) combatSelfTest = ToBool(*v, combatSelfTest);
		if (auto v = get("debugwarpoutdoors")) debugWarpOutdoors = ToBool(*v, debugWarpOutdoors);

		LC_LOG("config: Puppet=%d CameraMode=%s FovMode=%s MenuKey=%s (dik 0x%02X) Diagnostics=%d LogPerf=%d FreezePed=%d RootToFeet=%.2f (measure %d) ProbeFrom=%s ProbeHeight=%.1f CameraRows=%s",
			puppet, cameraMode == CameraMode::kScripted ? "scripted" : "final", fovMode == FovMode::kHorizontal43 ? "horizontal43" : "vertical",
			menuKey.c_str(), MenuKeyDik(), diagnostics, logPerf, freezePed, rootToFeet, measureRootToFeet, probeFrom == ProbeFrom::kFeet ? "feet" : "top", probeHeight, cameraRows.c_str());
		static constexpr const char* kRenderCameras[] = { "auto", "phase", "current", "finalcam" };
		static constexpr const char* kRenderDepths[] = { "auto", "log", "standard", "off" };
		static constexpr const char* kOverlayModes[] = { "auto", "always", "off" };
		LC_LOG("config: Render=%d RenderCamera=%s RenderDepth=%s RenderExposure=%.2f Overlay=%s", render, kRenderCameras[static_cast<int>(renderCamera)],
			kRenderDepths[static_cast<int>(renderDepth)], renderExposure, kOverlayModes[static_cast<int>(overlay)]);
		LC_LOG("config: Combat=%d PedDamageScale=%.1f PlayerDamageScale=%.1f ExplosionType=%d ExplosionRadiusScale=%.2f RagdollOnHit=%d%s%s", combat,
			pedDamageScale, playerDamageScale, explosionType, explosionRadiusScale, ragdollOnHit, combatSelfTest ? " CombatSelfTest=1" : "",
			debugWarpOutdoors ? " DebugWarpOutdoors=1" : "");
	}

	std::uint8_t Config::MenuKeyDik() const
	{
		const auto key = Lower(Trim(menuKey));
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

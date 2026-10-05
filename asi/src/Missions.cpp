// Unity-built into dllmain.cpp (needs IV-SDK). See Missions.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "missions"
#include "Missions.h"

#include "Config.h"
#include "Coords.h"
#include "Log.h"
#include "Game.h"
#include "Input.h"
#include "drive/Prompt.h"
#include "drive/SceneLogic.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace lc::Missions
{
	namespace
	{
		namespace S = ::Scripting;

		const Config& Cfg() { return Config::Get(); }

		// ---- what GTA IV 1.0.8.0 keeps where (each address is read from code that is checked first) ------
		// The player's control flags (CPlayerInfo +0x4BC): IS_PLAYER_CONTROL_ON is "all clear". SET_PLAYER_CONTROL
		// sets or clears bit 0x20 (0x8B5C80, called from the native's body 0xB34BA0; its network twin uses
		// 0x800), MAKE_PLAYER_SAFE_FOR_CUTSCENE sets 0x80 (0xB351B0); the other bits are the game's own.
		constexpr std::uint32_t kControlScript = 0x20;
		constexpr std::uint32_t kControlSafeForCutscene = 0x80;
		// ACTIVATE_SCRIPTED_CAMS's queued command (0x845700) keeps its first argument in a byte (0x11C738A):
		// a script's camera is what the game draws.
		constexpr std::uint32_t kScriptedCamsCode = 0x845747;  // mov al,[esi+8]; mov [kScriptedCams],al
		constexpr std::uint32_t kScriptedCams = 0x11C738A;
		// IS_MINIGAME_IN_PROGRESS (0xB4F1A0): a counter above 0.
		constexpr std::uint32_t kMinigameCode = 0xB4F1A0;    // xor eax,eax; cmp [kMinigames],eax; setg al; ret
		constexpr std::uint32_t kMinigames = 0x120A680;
		// A script's task on a ped (TASK_* natives: an event the ped's intelligence turns into its primary
		// task; GET_SCRIPT_TASK_STATUS reads it): the task's type in a word at intelligence +0x2EC (-1:
		// none), its status in the byte after. 0x96E0D0 is the game's own "has a script task" test.
		constexpr std::uint32_t kScriptTaskCode = 0x96E0D0;
		constexpr std::size_t   kIntelligence = 0x224;  // CPed: its CPedIntelligence
		constexpr std::size_t   kScriptTaskType = 0x2EC;
		constexpr std::size_t   kScriptTaskStatus = 0x2EE;
		// The help text box (0xF38668, what PRINT_HELP fills; IS_HELP_MESSAGE_BEING_DISPLAYED, 0xB54EC0, is
		// its +0x9A8 not 0, 0x8AAEB0) and its text (UTF-16 at +0x4B0, what IS_THIS_HELP_MESSAGE_BEING_DISPLAYED
		// compares, 0x8AAFCA), its tokens as in the GXT (~INPUT_PICKUP~).
		constexpr std::uint32_t kHelpCode = 0xB54EC0;  // mov ecx,kHelp; jmp 0x8AAEB0
		constexpr std::uint32_t kHelp = 0xF38668;
		constexpr std::uint32_t kHelpShownCode = 0x8AAEB0;
		constexpr std::uint32_t kHelpTextCode = 0x8AAFCA;
		constexpr std::size_t   kHelpShown = 0x9A8;
		constexpr std::size_t   kHelpText = 0x4B0;
		constexpr std::size_t   kHelpTextChars = (kHelpShown - kHelpText) / 2;
		// IS_PED_A_MISSION_PED: CPed::m_nCreatedBy == 2 (0xB41F70).
		constexpr std::uint8_t kCreatedByMission = 2;

		struct Addresses
		{
			bool          checked = false;
			const std::uint8_t* scriptedCams = nullptr;
			const std::int32_t* minigames = nullptr;
			bool          scriptTask = false;
			const std::uint8_t* help = nullptr;
		} addr;

		const std::uint8_t* Abs(std::uint32_t a_abs)
		{
			return reinterpret_cast<const std::uint8_t*>(AddressSetter::gBaseAddress + (a_abs - 0x400000u));
		}

		std::uint32_t Rel32(const std::uint8_t* a_p)
		{
			std::uint32_t v = 0;
			std::memcpy(&v, a_p, sizeof v);
			return v;
		}

		// The base-relative address a_p holds as an absolute 0x400000-based one.
		bool HoldsAbs(const std::uint8_t* a_p, std::uint32_t a_abs)
		{
			return Rel32(a_p) - AddressSetter::gBaseAddress == a_abs - 0x400000u;
		}

		void CheckAddresses()
		{
			if (addr.checked) {
				return;
			}
			addr.checked = true;
			if (plugin::gameVer != plugin::VERSION_1080) {
				LC_LOG("mission scenes: not 1.0.8.0: the player's control flags and IS_MINIGAME_IN_PROGRESS only (no script camera, no script tasks)");
				return;
			}
			const std::uint8_t* cams = Abs(kScriptedCamsCode);
			if (cams[0] == 0x8A && cams[1] == 0x46 && cams[2] == 0x08 && cams[3] == 0xA2 && HoldsAbs(cams + 4, kScriptedCams)) {
				addr.scriptedCams = Abs(kScriptedCams);
			}
			const std::uint8_t* mg = Abs(kMinigameCode);
			if (mg[0] == 0x33 && mg[1] == 0xC0 && mg[2] == 0x39 && mg[3] == 0x05 && HoldsAbs(mg + 4, kMinigames) && mg[8] == 0x0F && mg[9] == 0x9F) {
				addr.minigames = reinterpret_cast<const std::int32_t*>(Abs(kMinigames));
			}
			static constexpr std::uint8_t kTask[] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x88, 0x24, 0x02, 0x00, 0x00, 0x33, 0xC0, 0x66, 0x83, 0xB9, 0xEC, 0x02, 0x00, 0x00, 0xFF };
			addr.scriptTask = std::memcmp(Abs(kScriptTaskCode), kTask, sizeof kTask) == 0;
			static constexpr std::uint8_t kShown[] = { 0x33, 0xC0, 0x39, 0x81, 0xA8, 0x09, 0x00, 0x00, 0x0F, 0x95, 0xC0, 0xC3 };
			static constexpr std::uint8_t kText[] = { 0x81, 0xC6, 0xB0, 0x04, 0x00, 0x00 };
			const std::uint8_t* hc = Abs(kHelpCode);
			if (hc[0] == 0xB9 && HoldsAbs(hc + 1, kHelp) && std::memcmp(Abs(kHelpShownCode), kShown, sizeof kShown) == 0 &&
				std::memcmp(Abs(kHelpTextCode), kText, sizeof kText) == 0) {
				addr.help = Abs(kHelp);
			}
			LC_LOG("mission scenes: script camera flag %s, minigame counter %s, script task status %s, help text %s", addr.scriptedCams ? "found" : "NOT where 1.0.8.0 has it",
				addr.minigames ? "found" : "NOT where 1.0.8.0 has it (IS_MINIGAME_IN_PROGRESS instead)", addr.scriptTask ? "found" : "NOT where 1.0.8.0 has it",
				addr.help ? "found" : "NOT where 1.0.8.0 has it (no context actions in Minecraft mode)");
		}

		CPlayerInfo* Info(int a_player)
		{
			return a_player >= 0 && a_player < 32 ? CPlayerInfo::GetPlayerInfo(static_cast<std::uint32_t>(a_player)) : nullptr;
		}

		// The player's script task: its type (-1 none) and status, or false if unknown.
		bool ScriptTask(int a_ped, std::int16_t& a_type, std::int8_t& a_status)
		{
			a_type = -1;
			a_status = 0;
			if (!addr.scriptTask || !CPools::ms_pPedPool) {
				return false;
			}
			CPed* p = CPools::ms_pPedPool->GetAt(static_cast<std::uint32_t>(a_ped));
			if (!p) {
				return false;
			}
			const auto* intel = *reinterpret_cast<const std::uint8_t* const*>(reinterpret_cast<const std::uint8_t*>(p) + kIntelligence);
			if (!intel) {
				return false;
			}
			std::memcpy(&a_type, intel + kScriptTaskType, sizeof a_type);
			std::memcpy(&a_status, intel + kScriptTaskStatus, sizeof a_status);
			return true;
		}

		// ---- this frame's view of the game ------------------------------------------------------------------
		struct Snapshot
		{
			std::uint32_t control = 0;  // CPlayerInfo control flags (0: control on)
			bool          cams = false;
			int           minigames = 0;
			std::int16_t  taskType = -1;
			std::int8_t   taskStatus = 0;
			bool          cutscene = false;
			bool          phoneCall = false;
			bool          onMission = false;
			bool operator==(const Snapshot& o) const
			{
				return control == o.control && cams == o.cams && minigames == o.minigames && taskType == o.taskType && taskStatus == o.taskStatus &&
				       cutscene == o.cutscene && phoneCall == o.phoneCall && onMission == o.onMission;
			}
		};

		drive::SceneLogic logic;
		State             state;
		Snapshot          last{};
		bool              haveLast = false;
		std::uint32_t     frameNo = 0;
		std::uint32_t     lastHelpHash = 0;
		bool              lastShown = false;  // a scene was shown last frame

		void LogSnapshot(const Snapshot& a_s, const char* a_mode)
		{
			char bits[96] = "";
			if (a_s.control) {
				std::snprintf(bits, sizeof bits, " (%s%s%s)", (a_s.control & kControlScript) ? "script " : "", (a_s.control & kControlSafeForCutscene) ? "safe-for-cutscene " : "",
					(a_s.control & ~(kControlScript | kControlSafeForCutscene)) ? "game" : "");
			}
			LC_LOG("mission state: control flags 0x%X%s, script camera %d, minigames %d, script task %d status %d, cutscene %d, phone call %d, on a mission %d (%s)", a_s.control,
				bits, a_s.cams ? 1 : 0, a_s.minigames, a_s.taskType, a_s.taskStatus, a_s.cutscene ? 1 : 0, a_s.phoneCall ? 1 : 0, a_s.onMission ? 1 : 0, a_mode);
		}

		// ---- mission characters (memoised per frame) -----------------------------------------------------------
		std::unordered_map<int, bool> pedMemo, vehMemo;
		std::uint32_t                 memoFrame = 0;

		void Memo()
		{
			if (memoFrame != frameNo) {
				memoFrame = frameNo;
				pedMemo.clear();
				vehMemo.clear();
			}
		}

		bool MissionPedObj(const CPed* a_p)
		{
			return a_p && a_p->m_nCreatedBy == kCreatedByMission;
		}

		// ---- mob blasts: mission characters are explosion-proof for a moment -------------------------------------
		constexpr float kShieldSeconds = 2.0f;  // GTA's blast does its damage over its first frames
		struct Shield
		{
			int   handle;
			bool  vehicle;
			bool  hadProof;
			float left;
		};
		std::vector<Shield> shields;

		CPhysical* Physical(int a_handle, bool a_vehicle)
		{
			if (a_vehicle) {
				return CPools::ms_pVehiclePool ? reinterpret_cast<CPhysical*>(CPools::ms_pVehiclePool->GetAt(static_cast<std::uint32_t>(a_handle))) : nullptr;
			}
			return CPools::ms_pPedPool ? reinterpret_cast<CPhysical*>(CPools::ms_pPedPool->GetAt(static_cast<std::uint32_t>(a_handle))) : nullptr;
		}

		void ShieldOne(int a_handle, bool a_vehicle)
		{
			for (auto& s : shields) {
				if (s.handle == a_handle && s.vehicle == a_vehicle) {
					s.left = kShieldSeconds;
					return;
				}
			}
			CPhysical* p = Physical(a_handle, a_vehicle);
			if (!p || shields.size() >= 32) {
				return;
			}
			shields.push_back({ a_handle, a_vehicle, p->m_nPhysicalFlags.bExplosionProof != 0, kShieldSeconds });
			p->m_nPhysicalFlags.bExplosionProof = 1;
		}

		void ShieldTick(float a_dt)
		{
			for (auto it = shields.begin(); it != shields.end();) {
				if ((it->left -= a_dt) > 0.0f) {
					++it;
					continue;
				}
				const bool alive = it->vehicle ? S::DOES_VEHICLE_EXIST(it->handle) : S::DOES_CHAR_EXIST(it->handle);
				if (CPhysical* p = alive ? Physical(it->handle, it->vehicle) : nullptr) {
					p->m_nPhysicalFlags.bExplosionProof = it->hadProof ? 1 : 0;
				}
				it = shields.erase(it);
			}
		}

		// ---- test hooks ---------------------------------------------------------------------------------------
		// DebugMissionBlips: the radar's blips once, 10 s into play (where the contacts' missions start).
		const char* SpriteName(std::uint32_t a_sprite)
		{
			switch (a_sprite) {
			case SPRITE_VLAD: return "vlad";
			case SPRITE_MANNY: return "manny";
			case SPRITE_LITTLEJACOB: return "jacob";
			case SPRITE_ROMAN: return "roman";
			case SPRITE_FAUSTIN: return "faustin";
			case SPRITE_BERNIECRANE: return "bernie";
			case SPRITE_BRUCIE: return "brucie";
			case SPRITE_CIA: return "cia";
			case SPRITE_DWAYNE: return "dwayne";
			case SPRITE_ELIZABETA: return "elizabeta";
			case SPRITE_GAMBETTI: return "gambetti";
			case SPRITE_JIMMY: return "jimmy";
			case SPRITE_MCDERRICK: return "derrick";
			case SPRITE_MCFRANCIS: return "francis";
			case SPRITE_MCGERRY: return "gerry";
			case SPRITE_MCKATIE: return "katie";
			case SPRITE_MCPACKIE: return "packie";
			case SPRITE_PHILBELL: return "phil";
			case SPRITE_PLAYBOYX: return "playboy";
			case SPRITE_RAYBOCCINO: return "ray";
			case SPRITE_DIMITRI: return "dimitri";
			case SPRITE_MICHELLE: return "michelle";
			case SPRITE_SAFEHOUSE: return "safehouse";
			case SPRITE_BOWLING: return "bowling";
			case SPRITE_RESTAURANT: return "restaurant";
			case SPRITE_GIRLFRIEND: return "girlfriend";
			default: return nullptr;
			}
		}

		struct Blip
		{
			std::uint32_t sprite, colour, display;
			float         x, y, z;
		};

		std::vector<Blip> Blips()
		{
			std::vector<Blip> out;
			sRadarTrace** traces = CRadar::RadarTrace;
			if (!traces) {
				return out;
			}
			for (int i = 0; i < 1500; ++i) {
				const sRadarTrace* t = traces[i];
				if (!t) {
					continue;
				}
				const std::uint32_t sprite = t->m_pProperties ? t->m_pProperties->m_nSprite : 0xFFFFFFFFu;
				out.push_back({ sprite, t->m_nColour, t->m_nDisplay, t->m_vPos.x, t->m_vPos.y, t->m_vPos.z });
			}
			return out;
		}

		void DumpBlips(int a_ped)
		{
			float px = 0, py = 0, pz = 0;
			S::GET_CHAR_COORDINATES(a_ped, &px, &py, &pz);
			const auto blips = Blips();
			LC_LOG("DebugMissionBlips: %zu blips (the player at %.1f %.1f %.1f)", blips.size(), px, py, pz);
			for (const auto& b : blips) {
				const char* name = SpriteName(b.sprite);
				LC_LOG("  blip sprite %u%s%s colour %u display %u at %.1f %.1f %.1f (%.0f m)", b.sprite, name ? " " : "", name ? name : "", b.colour, b.display, b.x, b.y, b.z,
					std::hypot(b.x - px, b.y - py));
			}
		}

		// DebugMissionWarp=<names>: 20 s into play the player is put next to the nearest blip of one of those
		// contacts (e.g. "roman,vlad", "any" for every contact), where its mission starts.
		bool probeDone();  // (DebugMissionProbe, below: the warp waits for it)

		struct Warp
		{
			float t = 0.0f;
			int   step = 0;  // 0 waiting, 1 loading the scene there, 2 there, control off a moment more, 3 done
			float to[3]{};
			float wait = 0.0f;
		} warp;

		void WarpTick(const Frame& a_f)
		{
			if (Cfg().debugMissionWarp.empty() || warp.step >= 3 || !a_f.exists || a_f.loading || a_f.paused || a_f.dead) {
				return;
			}
			if (warp.step == 0) {
				if (!probeDone()) {
					warp.t = 15.0f;  // (5 s after the probe)
					return;
				}
				if ((warp.t += a_f.dt) < 20.0f || a_f.inCar) {
					return;
				}
				float px = 0, py = 0, pz = 0;
				S::GET_CHAR_COORDINATES(a_f.ped, &px, &py, &pz);
				const std::string& want = Cfg().debugMissionWarp;
				float      best = 1e30f;
				const Blip* pick = nullptr;
				const auto blips = Blips();
				for (const auto& b : blips) {
					const char* name = SpriteName(b.sprite);
					if (!name || ((std::strcmp(name, "safehouse") == 0 || std::strcmp(name, "bowling") == 0 || std::strcmp(name, "girlfriend") == 0 ||
									  std::strcmp(name, "restaurant") == 0) && want == "any")) {
						continue;
					}
					if (want != "any" && want.find(name) == std::string::npos) {
						continue;
					}
					const float d = std::hypot(b.x - px, b.y - py);
					if (d < best) {
						best = d, pick = &b;
					}
				}
				if (!pick) {
					LC_LOG("DebugMissionWarp: no %s blip on the radar", want.c_str());
					warp.step = 3;
					return;
				}
				warp.to[0] = pick->x, warp.to[1] = pick->y, warp.to[2] = pick->z;
				// As a script warps the player: his control off meanwhile (in Minecraft mode GTA gets him, so
				// puppet mode doesn't put him back where Minecraft has him).
				S::SET_PLAYER_CONTROL(a_f.player, false);
				S::REQUEST_COLLISION_AT_POSN(pick->x, pick->y, pick->z);
				S::LOAD_SCENE(pick->x, pick->y, pick->z);
				LC_LOG("DebugMissionWarp: to %s's blip at %.1f %.1f %.1f (%.0f m away)", SpriteName(pick->sprite), pick->x, pick->y, pick->z, best);
				warp.step = 1;
				warp.wait = 0.0f;
				return;
			}
			if ((warp.wait += a_f.dt) < 1.0f) {
				return;
			}
			if (warp.step == 2) {
				if (warp.wait >= 2.5f) {
					S::SET_PLAYER_CONTROL(a_f.player, true);
					LC_LOG("DebugMissionWarp: the player's control is back on");
					warp.step = 3;
				}
				return;
			}
			float ground = 0.0f;
			S::GET_GROUND_Z_FOR_3D_COORD(warp.to[0], warp.to[1], warp.to[2] + 2.0f, &ground);
			const float z = ground != 0.0f ? ground + 1.0f : warp.to[2] + 1.0f;
			S::SET_CHAR_COORDINATES(a_f.ped, warp.to[0], warp.to[1], z);
			LC_LOG("DebugMissionWarp: the player is at %.1f %.1f %.1f (ground %.1f)", warp.to[0], warp.to[1], z, ground);
			warp.step = 2;  // (warp.wait goes on: control back on 1.5 s later)
		}

		// DebugMissionProbe=<s>: s seconds into play (on foot) a mission scene as a script plays one: the player
		// is told to walk 4 m ahead (TASK_GO_STRAIGHT_TO_COORD; a script waits for that), his control goes off for
		// 3 s, then a camera of ours looks at him for 4 s (ACTIVATE_SCRIPTED_CAMS). Each step logs what happened;
		// the steps that are screenshot moments log "DebugMissionProbe: SCREENSHOT".
		struct Probe
		{
			float t = 0.0f, stepT = 0.0f, logT = 0.0f;
			int   step = 0;  // 0 wait, 1 walk, 2 control off, 3 camera, 4 minigame, 5 done
			float from[3]{};
			int   cam = 0;
			bool  shot = false;
			int   round = 0;     // DebugMissionProbeAB: 0 with ScriptScenes off (as before), 1 on
			bool  scenesOff = false;
		} probe;

		// DebugProp=<model>: 10 s into play (on foot) an object of that model is attached to Niko's right hand and
		// his control goes off for 6 s (a mission script has him: the Minecraft body on him), logged with a
		// SCREENSHOT line; then the control comes back and the object goes.
		struct Prop
		{
			float t = 0.0f;
			int   step = 0;  // 0 wait, 1 loading the model, 2 attached (control off), 3 done
			int   obj = 0;
			bool  shot = false;
		} prop;

		void PropTick(const Frame& a_f)
		{
			if (Cfg().debugProp.empty() || prop.step >= 3 || !a_f.exists || a_f.loading || a_f.paused || a_f.dead) {
				return;
			}
			const unsigned model = S::GET_HASH_KEY(Cfg().debugProp.c_str());
			prop.t += a_f.dt;
			if (prop.step == 0) {
				if (prop.t < 10.0f || a_f.inCar || !a_f.puppeting) {
					return;
				}
				CStreaming::ScriptRequestModel(static_cast<std::int32_t>(model));
				prop.step = 1;
				prop.t = 0.0f;
				LC_LOG("DebugProp: requesting %s", Cfg().debugProp.c_str());
				return;
			}
			if (prop.step == 1) {
				if (!S::HAS_MODEL_LOADED(model)) {
					if (prop.t > 10.0f) {
						LC_LOG("DebugProp: %s didn't load", Cfg().debugProp.c_str());
						prop.step = 3;
					}
					return;
				}
				float x = 0, y = 0, z = 0;
				S::GET_CHAR_COORDINATES(a_f.ped, &x, &y, &z);
				S::CREATE_OBJECT(model, x, y, z + 2.0f, &prop.obj, true);
				S::MARK_MODEL_AS_NO_LONGER_NEEDED(model);
				if (prop.obj) {
					S::ATTACH_OBJECT_TO_PED(prop.obj, a_f.ped, 0x4D0 /* right hand */, 0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0);
					S::SET_PLAYER_CONTROL(a_f.player, false);
				}
				LC_LOG("DebugProp: object %d (%s) attached to Niko's right hand; his control off for 6 s", prop.obj, Cfg().debugProp.c_str());
				prop.step = prop.obj ? 2 : 3;
				prop.t = 0.0f;
				return;
			}
			if (!prop.shot && prop.t > 2.5f) {
				prop.shot = true;
				LC_LOG("DebugProp: SCREENSHOT (attached %d, scripted %d)", S::IS_OBJECT_ATTACHED(prop.obj) ? 1 : 0, state.scripted ? 1 : 0);
			}
			if (prop.t < 6.0f) {
				return;
			}
			S::SET_PLAYER_CONTROL(a_f.player, true);
			if (S::DOES_OBJECT_EXIST(prop.obj)) {
				S::DETACH_OBJECT(prop.obj, true);
				S::DELETE_OBJECT(&prop.obj);
			}
			LC_LOG("DebugProp: done");
			prop.step = 3;
		}

		bool probeDone()
		{
			return Cfg().debugMissionProbe <= 0.0f || probe.step >= 5;
		}

		void ProbeTick(const Frame& a_f)
		{
			if (Cfg().debugMissionProbe <= 0.0f || probe.step >= 5 || !a_f.exists || a_f.loading || a_f.paused || a_f.dead) {
				return;
			}
			const float dt = a_f.dt;
			std::int16_t type = -1;
			std::int8_t  status = 0;
			ScriptTask(a_f.ped, type, status);
			float x = 0, y = 0, z = 0;
			S::GET_CHAR_COORDINATES(a_f.ped, &x, &y, &z);
			if (probe.step == 0) {
				probe.scenesOff = Cfg().debugMissionProbeAB && probe.round == 0;
				if (a_f.inCar || (probe.t += dt) < Cfg().debugMissionProbe) {
					return;
				}
				if (Cfg().debugMissionProbeAB) {
					LC_LOG("DebugMissionProbe: round %s", probe.round == 0 ? "A: ScriptScenes off (puppet mode as before)" : "B: ScriptScenes on");
				}
				float h = 0;
				S::GET_CHAR_HEADING(a_f.ped, &h);
				const float hx = -std::sin(h * kDegToRad), hy = std::cos(h * kDegToRad);
				probe.from[0] = x, probe.from[1] = y, probe.from[2] = z;
				S::TASK_GO_STRAIGHT_TO_COORD(a_f.ped, x + hx * 4.0f, y + hy * 4.0f, z, 2, 8000);
				ScriptTask(a_f.ped, type, status);
				LC_LOG("DebugMissionProbe: step 1: TASK_GO_STRAIGHT_TO_COORD 4 m ahead (heading %.0f); script task now %d status %d, puppeting %d", h, type, status,
					a_f.puppeting ? 1 : 0);
				probe.step = 1;
				probe.stepT = probe.logT = 0.0f;
				return;
			}
			probe.stepT += dt;
			if (probe.step == 1) {
				const float moved = std::hypot(x - probe.from[0], y - probe.from[1]);
				if ((probe.logT -= dt) <= 0.0f) {
					probe.logT = 0.5f;
					LC_LOG("DebugMissionProbe: walk %.1f s: script task %d status %d, moved %.2f m, puppeting %d, scripted %d", probe.stepT, type, status, moved,
						a_f.puppeting ? 1 : 0, state.scripted ? 1 : 0);
				}
				if (!probe.shot && probe.stepT > 1.0f) {
					probe.shot = true;
					LC_LOG("DebugMissionProbe: SCREENSHOT walk");
				}
				if (type == -1 && probe.stepT > 0.3f) {
					LC_LOG("DebugMissionProbe: the walk task ended after %.1f s, %.2f m from the start (%s)", probe.stepT, moved, moved > 3.0f ? "it walked" : "it did NOT walk");
				} else if (probe.stepT < 12.0f) {
					return;
				} else {
					LC_LOG("DebugMissionProbe: the walk task is STUCK after %.1f s (script task %d status %d, moved %.2f m): a script waiting for it would soft lock",
						probe.stepT, type, status, moved);
				}
				S::SET_PLAYER_CONTROL(a_f.player, false);
				LC_LOG("DebugMissionProbe: step 2: SET_PLAYER_CONTROL off for 3 s");
				probe.step = 2;
				probe.stepT = 0.0f;
				probe.shot = false;
				return;
			}
			if (probe.step == 2) {
				if (!probe.shot && probe.stepT > 1.5f) {
					probe.shot = true;
					LC_LOG("DebugMissionProbe: control off 1.5 s: IS_PLAYER_CONTROL_ON %d, puppeting %d, scripted %d", S::IS_PLAYER_CONTROL_ON(a_f.player) ? 1 : 0,
						a_f.puppeting ? 1 : 0, state.scripted ? 1 : 0);
				}
				if (probe.stepT < 3.0f) {
					return;
				}
				S::SET_PLAYER_CONTROL(a_f.player, true);
				float h = 0;
				S::GET_CHAR_HEADING(a_f.ped, &h);
				const float hx = -std::sin(h * kDegToRad), hy = std::cos(h * kDegToRad);
				S::CREATE_CAM(14, &probe.cam);
				if (probe.cam) {
					S::SET_CAM_POS(probe.cam, x + hx * 3.5f + hy * 1.0f, y + hy * 3.5f - hx * 1.0f, z + 0.6f);
					S::POINT_CAM_AT_COORD(probe.cam, x, y, z + 0.3f);
					S::SET_CAM_ACTIVE(probe.cam, true);
					S::SET_CAM_PROPAGATE(probe.cam, true);
					S::ACTIVATE_SCRIPTED_CAMS(true, true);
				}
				LC_LOG("DebugMissionProbe: step 3: control back on; a script camera %d in front of the player for 4 s", probe.cam);
				probe.step = 3;
				probe.stepT = 0.0f;
				probe.shot = false;
				return;
			}
			if (probe.step == 3) {
				if (!probe.shot && probe.stepT > 1.5f) {
					probe.shot = true;
					LC_LOG("DebugMissionProbe: SCREENSHOT camera (puppeting %d, scripted %d, scene %d)", a_f.puppeting ? 1 : 0, state.scripted ? 1 : 0, state.scene ? 1 : 0);
				}
				if (probe.stepT < 4.0f) {
					return;
				}
				if (probe.cam) {
					S::SET_CAM_ACTIVE(probe.cam, false);
					S::ACTIVATE_SCRIPTED_CAMS(false, false);
					S::DESTROY_CAM(probe.cam);
					probe.cam = 0;
				}
				S::SET_MINIGAME_IN_PROGRESS(true);
				LC_LOG("DebugMissionProbe: step 4: the camera is gone; a minigame for 3 s (SET_MINIGAME_IN_PROGRESS)");
				probe.step = 4;
				probe.stepT = 0.0f;
				probe.shot = false;
				return;
			}
			if (probe.step == 4) {
				if (!probe.shot && probe.stepT > 1.5f) {
					probe.shot = true;
					LC_LOG("DebugMissionProbe: minigame 1.5 s: IS_MINIGAME_IN_PROGRESS %d, puppeting %d, scripted %d", S::IS_MINIGAME_IN_PROGRESS() ? 1 : 0,
						a_f.puppeting ? 1 : 0, state.scripted ? 1 : 0);
				}
				if (probe.stepT < 3.0f) {
					return;
				}
				S::SET_MINIGAME_IN_PROGRESS(false);
				LC_LOG("DebugMissionProbe: done%s", Cfg().debugMissionProbeAB ? (probe.round == 0 ? " (round A, ScriptScenes off)" : " (round B, ScriptScenes on)") : "");
				probe.step = 5;
				if (Cfg().debugMissionProbeAB && probe.round == 0) {
					// Round B in 8 s (Minecraft has the player back by then), with the takeover on.
					probe.round = 1;
					probe.scenesOff = false;
					probe.step = 0;
					probe.shot = false;
					probe.t = Cfg().debugMissionProbe - 8.0f;
				}
			}
		}
	}

	State Tick(const Frame& a_f)
	{
		++frameNo;
		CheckAddresses();
		const bool inGame = a_f.exists && !a_f.loading;
		if (inGame) {
			// (Test hooks first: what they do to the player is seen this frame, as a mission script's natives
			// are, before puppet mode moves him.)
			WarpTick(a_f);
			ProbeTick(a_f);
			PropTick(a_f);
		}
		Snapshot s;
		if (inGame) {
			if (CPlayerInfo* info = Info(a_f.player)) {
				s.control = info->m_nControlFlags;
			}
			s.cams = addr.scriptedCams ? *addr.scriptedCams != 0 : false;
			s.minigames = addr.minigames ? *addr.minigames : (S::IS_MINIGAME_IN_PROGRESS() ? 1 : 0);
			ScriptTask(a_f.ped, s.taskType, s.taskStatus);
			s.cutscene = a_f.cutscene;
			s.phoneCall = S::IS_MOBILE_PHONE_CALL_ONGOING();
			s.onMission = S::GET_MISSION_FLAG();
		}
		// GTA's help text offering context actions and menu choices (drive/Prompt.h): Input lets their controls
		// through and keeps their keys from Minecraft while it shows.
		unsigned prompt = 0;
		if (inGame && addr.help && *reinterpret_cast<const std::int32_t*>(addr.help + kHelpShown) != 0) {
			const auto* text = reinterpret_cast<const std::uint16_t*>(addr.help + kHelpText);
			prompt = drive::PromptActions(text, kHelpTextChars);
			std::uint32_t hash = 2166136261u;
			for (std::size_t i = 0; i < kHelpTextChars && text[i]; ++i) {
				hash = (hash ^ text[i]) * 16777619u;
			}
			if (hash != lastHelpHash) {
				lastHelpHash = hash;
				char ascii[160];
				std::size_t n = 0;
				for (std::size_t i = 0; i < kHelpTextChars && text[i] && n < sizeof(ascii) - 1; ++i) {
					ascii[n++] = text[i] < 0x80 && text[i] >= 0x20 ? static_cast<char>(text[i]) : '?';
				}
				ascii[n] = 0;
				LC_LOG("GTA's help text: \"%s\" (offers%s%s%s%s)", ascii, (prompt & drive::kPromptPickup) ? " INPUT_PICKUP" : "",
					(prompt & drive::kPromptAccept) ? " FRONTEND_ACCEPT" : "", (prompt & drive::kPromptCancel) ? " FRONTEND_CANCEL" : "", prompt ? "" : " no menu choice");
			}
		} else {
			lastHelpHash = 0;
		}
		const unsigned action = Cfg().contextActions ? prompt : 0u;
		if (action != state.prompt) {
			LC_LOG("%s", action ? "context actions offered: GTA gets their controls (and their keys, not Minecraft) while they show" : "the context actions are gone");
		}
		state.prompt = action;
		Input::SetPrompt(action);
		// DebugFakePrompt=<s>: s seconds into puppet mode GTA's help box shows "Press ~INPUT_PICKUP~ ..." through
		// the game's own setter (what PRINT_HELP calls: 0x8ABA20 on the box, called at 0xB54A58, checked), as
		// a shop or the bowling alley would. While the context key is down (DebugContextKey) the pad's
		// INPUT_PICKUP is logged as scripts read it (IS_CONTROL_PRESSED), with Minecraft's screen state.
		static float fakeT = 0.0f;
		if (Cfg().debugFakePrompt > 0.0f && fakeT >= 0.0f && inGame && a_f.puppeting && (fakeT += a_f.paused ? 0.0f : a_f.dt) >= Cfg().debugFakePrompt) {
			fakeT = -1.0f;
			const std::uint8_t* call = Abs(0xB54A53);
			std::int32_t        rel = 0;
			std::memcpy(&rel, call + 6, sizeof rel);
			const auto target = reinterpret_cast<std::uintptr_t>(call + 10) + rel;
			if (addr.help && call[0] == 0xB9 && HoldsAbs(call + 1, kHelp) && call[5] == 0xE8 && target == reinterpret_cast<std::uintptr_t>(Abs(0x8ABA20))) {
				static std::uint16_t text[96];
				const char*          ascii = "Press ~INPUT_PICKUP~ to play a half game.~n~Press ~ACCEPT~ to play a full game.";
				std::size_t          n = 0;
				for (; ascii[n] && n < 95; ++n) {
					text[n] = static_cast<std::uint8_t>(ascii[n]);
				}
				text[n] = 0;
				using SetHelp = void(__thiscall*)(const void*, const std::uint16_t*, int, int, int, int, int, int, int, int, int, int, int, int, int);
				reinterpret_cast<SetHelp>(target)(addr.help, text, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, -1);
				LC_LOG("DebugFakePrompt: GTA's help box set");
			} else {
				LC_LOG("DebugFakePrompt: the help box setter isn't where 1.0.8.0 has it");
			}
		}
		const std::uint8_t testKey = Cfg().debugContextKey.empty() ? 0 : Config::KeyDik(Cfg().debugContextKey);
		if (testKey && inGame) {
			static unsigned wasDown = 0;
			const unsigned  down = (S::IS_CONTROL_PRESSED(0, INPUT_PICKUP) ? 1u : 0u) | (S::IS_CONTROL_PRESSED(0, INPUT_FRONTEND_ACCEPT) ? 2u : 0u) |
			                      (S::IS_CONTROL_PRESSED(0, INPUT_FRONTEND_CANCEL) ? 4u : 0u);
			if (down != wasDown) {
				wasDown = down;
				LC_LOG("DebugContextKey: GTA's pad reads INPUT_PICKUP %s, FRONTEND_ACCEPT %s, FRONTEND_CANCEL %s (puppeting %d, a Minecraft screen open %d)",
					(down & 1) ? "DOWN" : "up", (down & 2) ? "DOWN" : "up", (down & 4) ? "DOWN" : "up", a_f.puppeting ? 1 : 0, Game::State().mcScreenOpen.load() ? 1 : 0);
			}
		}
		// DebugContextKey=<key>: 1.5 s into the first offered context actions while puppeting, that key is
		// pressed (a real key event) for 0.15 s.
		static float keyT = 0.0f;
		static int   keyStep = 0;  // 0 waiting, 1 held, 2 done
		if (testKey && keyStep < 2) {
			keyT = (action && a_f.puppeting) || keyStep == 1 ? keyT + (a_f.paused ? 0.0f : a_f.dt) : 0.0f;
			if (keyStep == 0 && keyT >= 1.5f) {
				Input::SendTestKey(testKey, true);
				keyStep = 1;
				keyT = 0.0f;
				LC_LOG("DebugContextKey: %s down (offered bits 0x%X)", Cfg().debugContextKey.c_str(), action);
			} else if (keyStep == 1 && keyT >= 0.15f) {
				Input::SendTestKey(testKey, false);
				keyStep = 2;
				LC_LOG("DebugContextKey: %s up", Cfg().debugContextKey.c_str());
			}
		}

		// Our own CameraMode=scripted camera is no mission's.
		const bool scriptCam = s.cams && a_f.ownScriptCam == 0;

		unsigned signals = 0;
		if (inGame && !a_f.dead) {
			// (Without PuppetPlayerControl puppet mode switches the control off itself, with the script's own
			// bit: then only MAKE_PLAYER_SAFE_FOR_CUTSCENE's counts.)
			const std::uint32_t scriptBits = Cfg().puppetPlayerControl ? (kControlScript | kControlSafeForCutscene) : kControlSafeForCutscene;
			signals |= (s.control & scriptBits) ? drive::kSigControlOff : 0u;
			signals |= scriptCam ? drive::kSigScriptCamera : 0u;
			signals |= s.minigames > 0 ? drive::kSigMinigame : 0u;
			signals |= s.taskType != -1 ? drive::kSigScriptTask : 0u;
		}
		const bool eligible = Cfg().scriptScenes && !probe.scenesOff && inGame && !a_f.dead && !a_f.inCar && !a_f.cutscene && a_f.minecraftMode && a_f.mcInWorld;
		// (Fading out at a cutscene's end counts as not in game: the scene goes on until the game is back, so
		// Minecraft doesn't wake for the fade.)
		const bool shown = (inGame && (a_f.cutscene || scriptCam)) || (a_f.exists && a_f.loading && !a_f.dead && lastShown);
		lastShown = shown;
		const auto out = logic.Step(signals, a_f.paused ? 0.0f : a_f.dt, eligible, shown);

		const char* mode = !a_f.minecraftMode ? "Niko mode" : a_f.puppeting ? "puppeting" : a_f.inCar ? "in a vehicle" : "GTA drives";
		if (inGame && (!haveLast || !(s == last))) {
			LogSnapshot(s, mode);
			last = s;
			haveLast = true;
		}
		state.scripted = out.scripted;
		state.scene = out.scene && Cfg().scenesPauseMinecraft;
		state.phoneCall = inGame && s.phoneCall;
		// DebugPhoneCall=<s>: s seconds into play 6 s of a phone call as far as Minecraft is told (its duck).
		static float callT = 0.0f;
		if (Cfg().debugPhoneCall > 0.0f && inGame && callT >= 0.0f) {
			callT += a_f.paused ? 0.0f : a_f.dt;
			if (callT >= Cfg().debugPhoneCall + 6.0f) {
				callT = -1.0f;
				LC_LOG("DebugPhoneCall: over");
			} else if (callT >= Cfg().debugPhoneCall) {
				state.phoneCall = true;
			}
		}
		static char why[160];
		if (out.scripted) {
			int n = 0;
			why[0] = 0;
			for (unsigned bit = 1; bit < (1u << drive::kSigCount) && n < static_cast<int>(sizeof why) - 40; bit <<= 1) {
				if (out.signals & bit) {
					n += std::snprintf(why + n, sizeof why - static_cast<std::size_t>(n), "%s%s", n ? ", " : "", drive::SceneSignalName(bit));
				}
			}
			state.why = why;
		} else {
			state.why = nullptr;
		}
		if (out.started) {
			LC_LOG("a mission script has the player (%s): GTA IV drives him until it lets go", why);
		} else if (out.ended && eligible) {
			LC_LOG("the mission script let go of the player (%.2f s with nothing holding him): Minecraft takes him back", drive::SceneLogic::kReleaseSeconds);
		} else if (out.ended) {
			LC_LOG("the mission scene goes on as %s", a_f.inCar ? "a vehicle's (GTA drives)" : a_f.cutscene ? "a cutscene" : !a_f.minecraftMode ? "Niko mode" : "the game's");
		}
		static bool lastScene = false, lastCall = false;
		if (state.scene != lastScene) {
			lastScene = state.scene;
			LC_LOG("%s", state.scene ? "a scene is shown (a cutscene or a script's camera): Minecraft pauses" : "the scene is over: Minecraft goes on");
		}
		if (state.phoneCall != lastCall) {
			lastCall = state.phoneCall;
			LC_LOG("%s", state.phoneCall ? "a phone call: Minecraft's sounds duck" : "the phone call is over: Minecraft's sounds come back");
		}

		ShieldTick(a_f.paused ? 0.0f : a_f.dt);
		if (inGame) {
			static float blipsT = 0.0f;
			if (Cfg().debugMissionBlips && blipsT >= 0.0f && !a_f.paused && (blipsT += a_f.dt) >= 10.0f && a_f.exists) {
				blipsT = -1.0f;
				DumpBlips(a_f.ped);
			}
		}
		return state;
	}

	const State& Current()
	{
		return state;
	}

	void OnIngameStartup()
	{
		logic.Reset();
		state = State{};
		lastShown = false;
		haveLast = false;
		shields.clear();  // (the peds and vehicles are going away)
		pedMemo.clear();
		vehMemo.clear();
	}

	bool IsMissionPed(int a_ped)
	{
		if (!Cfg().missionPedsSafe || !a_ped || !CPools::ms_pPedPool) {
			return false;
		}
		Memo();
		if (const auto it = pedMemo.find(a_ped); it != pedMemo.end()) {
			return it->second;
		}
		const bool mission = MissionPedObj(CPools::ms_pPedPool->GetAt(static_cast<std::uint32_t>(a_ped)));
		pedMemo.emplace(a_ped, mission);
		return mission;
	}

	bool IsMissionVehicle(int a_vehicle)
	{
		if (!Cfg().missionPedsSafe || !a_vehicle || !CPools::ms_pVehiclePool) {
			return false;
		}
		Memo();
		if (const auto it = vehMemo.find(a_vehicle); it != vehMemo.end()) {
			return it->second;
		}
		bool       mission = false;
		CVehicle*  v = CPools::ms_pVehiclePool->GetAt(static_cast<std::uint32_t>(a_vehicle));
		if (v) {
			bool occupied = v->m_pDriver != nullptr;
			mission = MissionPedObj(v->m_pDriver);
			const unsigned seats = std::min<unsigned>(v->m_nMaxPassengers, 8u);
			for (unsigned i = 0; i < seats && !mission; ++i) {
				occupied = occupied || v->m_pPassengers[i];
				mission = MissionPedObj(v->m_pPassengers[i]);
			}
			// A mission's own car counts while someone sits in it (an empty one is no target anyway).
			if (!mission && occupied && S::IS_CAR_A_MISSION_CAR(a_vehicle)) {
				mission = !(v->m_pDriver && v->m_pDriver == FindPlayerPed());
			}
		}
		vehMemo.emplace(a_vehicle, mission);
		return mission;
	}

	void ShieldFromMobBlast(float a_x, float a_y, float a_z, float a_reach)
	{
		if (!Cfg().missionPedsSafe) {
			return;
		}
		const float r2 = a_reach * a_reach;
		int         peds = 0, vehicles = 0;
		CPed*       player = FindPlayerPed();
		if (auto* pool = CPools::ms_pPedPool) {
			for (int i = 0; i < static_cast<int>(pool->m_nCount); ++i) {
				CPed* p = pool->Get(i);
				if (!p || p == player || !p->m_pMatrix || !MissionPedObj(p)) {
					continue;
				}
				const auto& m = p->m_pMatrix->pos;
				if ((m.x - a_x) * (m.x - a_x) + (m.y - a_y) * (m.y - a_y) + (m.z - a_z) * (m.z - a_z) > r2) {
					continue;
				}
				ShieldOne(static_cast<int>(pool->GetIndex(p)), false);
				++peds;
			}
		}
		if (auto* pool = CPools::ms_pVehiclePool) {
			for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
				CVehicle* v = pool->Get(slot);
				if (!v || !v->m_pMatrix) {
					continue;
				}
				const auto& m = v->m_pMatrix->pos;
				if ((m.x - a_x) * (m.x - a_x) + (m.y - a_y) * (m.y - a_y) + (m.z - a_z) * (m.z - a_z) > r2) {
					continue;
				}
				const int handle = static_cast<int>(pool->GetIndex(v));
				// (A mission's own car counts empty too: the creeper that wrecked Jacob's car would fail it.)
				if (handle && (IsMissionVehicle(handle) || (Cfg().missionPedsSafe && S::IS_CAR_A_MISSION_CAR(handle)))) {
					ShieldOne(handle, true);
					++vehicles;
				}
			}
		}
		if (peds || vehicles) {
			LC_LOG("a Minecraft mob's blast at %.1f %.1f %.1f: %d mission character(s) and %d of their vehicle(s) within %.0f m explosion-proof for %.0f s", a_x, a_y, a_z,
				peds, vehicles, a_reach, kShieldSeconds);
		}
	}
}

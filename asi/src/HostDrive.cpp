// Unity-built into dllmain.cpp (needs IV-SDK). See HostDrive.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "drive"
#include "HostDrive.h"

#include "Config.h"
#include "DriveLogic.h"
#include "Game.h"
#include "Log.h"
#include "NikoBody.h"
#include "NpcBlocks.h"
#include "collision/Rays.h"
#include "combat/CombatMath.h"
#include "drive/VehicleHit.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace lc::HostDrive
{
	namespace
	{
		namespace S = ::Scripting;
		namespace hit = ::lc::drive::hit;

		constexpr float kFallbackRadius = 10.0f;     // VehicleEnterFallback looks this far for a car
		constexpr float kDebugCarRadius = 12.0f;     // DebugAutoVehicle: "near a car"
		constexpr float kDebugToggleSeconds = 10.0f;
		constexpr float kDebugExitAfter = 12.0f;     // DebugAutoVehicle: seconds in a car before getting out
		constexpr float kDebugCooldown = 25.0f;
		constexpr float kExitPressSeconds = 0.3f;

		const Config& Cfg() { return Config::Get(); }

		drive::Logic logic;
		bool         configured = false;
		const char*  lastReason = nullptr;

		// key presses from the window thread, taken by Tick
		std::atomic<int> togglePresses{ 0 };
		std::atomic<int> vehiclePresses{ 0 };
		// what Pad presses, set by Tick
		std::atomic<bool> pressEnter{ false };
		std::atomic<bool> pressExit{ false };
		std::atomic<bool> pressThrottle{ false };  // DebugDriveThrottle
		float             throttleT = 0.0f, throttleLogT = 0.0f;

		int   hiddenPed = 0;  // the ped we made invisible in a vehicle (0: none)
		int   seatLoggedFor = 0;
		float debugToggleT = 0.0f;
		float debugCooldown = 12.0f;  // after a load Niko may still be falling into place
		float debugInCarT = 0.0f;
		float exitPressT = 0.0f;
		float debugExitCheckT = 0.0f;  // DebugAutoVehicle: seconds until it checks the exit press worked
		int   debugCar = 0;  // DebugAutoVehicle: the empty test car it parked next to Niko
		bool  debugCarRequested = false;
		float debugIndoorT = 0.0f;  // seconds puppeting indoors / off the ground without a car near
		int   debugRelocate = 0;    // frames left of moving Niko to the road (puppet off meanwhile)
		bool  debugRelocated = false;
		float debugRelocateTo[3]{};
		// what the pad held for GTA's enter control before Pad touched it (diagnostics)
		std::atomic<int> padEnterCurrent{ 0 }, padEnterLast{ 0 };
		bool  sawGettingIn = false;
		float enterClock = 0.0f;  // seconds since the vehicle key (log)
		int   tapsThisAttempt = 0;
		bool  lastStanding = true;
		// Pad runs only for the pad GTA hands the player's ped, i.e. while player control is on:
		// frames in a row it ran = GTA reads the player's pad again (the taps wait for that).
		std::atomic<std::uint32_t> padFrames{ 0 };
		std::uint32_t              padFramesSeen = 0;
		int                        padLiveRun = 0;
		constexpr int              kPadLiveFrames = 3;
		constexpr float            kMovingSpeed = 0.6f;  // m/s on the ground: GTA walks Niko to a door
		constexpr float            kStandWait = 0.75f;   // the taps wait this long at most for Niko to stand
		float                      releasedT = 0.0f;     // seconds since puppet mode let go for this attempt

		// GTA's enter-vehicle check (1.0.8.0: 0xA60D0B, in the player's on-foot task) takes the press
		// only while the ped counts as standing: CPed flag word 0x26C bit 0, set by the ped's ground
		// probe (0x93E710) and cleared by every move of the SET_CHAR_COORDINATES kind (0x945F38), i.e.
		// on every puppet frame. After puppet mode lets go the ped stands again 0.1 to 0.5 s later; the
		// old single press came before that and was lost (the player's second F then worked).
		bool PedStanding()
		{
			const CPed* p = FindPlayerPed();
			return p && (*reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(p) + 0x26C) & 1u) != 0;
		}

		// ---- DebugWalkThroughCar (test hook) -----------------------------------------------------------
		// Walks the puppet target through a parked car and then into a pedestrian, first with the old
		// move (SET_CHAR_COORDINATES_NO_OFFSET), then with the direct one, and logs whether the car and
		// the ped survive. Both are made ambient (no longer needed) first: the clearing spares mission
		// entities, and the car the player walks into on the street is an ambient one.
		struct WalkTest
		{
			int           round = 0;  // 0 the native, 1 the direct move, 2 done
			int           step = 0;   // 0 wait, 1 spawn, 2 settle, 3 through the car, 4 into the ped, 5 back
			float         t = 0.0f, wait = 0.0f, dt = 0.0f;
			int           car = 0, ped = 0;
			bool          carGone = false, pedGone = false;
			float         carGoneAt = 0.0f, pedGoneAt = 0.0f;
			float         carClosest = 99.0f, pedClosest = 99.0f;  // the target's closest approach (m, 2D)
			bool          needStart = false, active = false, modelRequested = false;
			GtaVec        start{}, target{};
			float         dir[2]{};
			float         carPos[3]{};
			std::uint32_t vehBefore = 0, pedsBefore = 0;
		} walk;

		struct Car
		{
			int   handle = 0;
			float distance = 0.0f;
		};

		// The closest vehicle to the player within a_radius that isn't wrecked (the vehicle pool;
		// IV-SDK has no GET_CLOSEST_CAR).
		Car ClosestCar(int a_ped, float a_radius, bool a_emptyOnly = false)
		{
			constexpr float kMaxDz = 4.0f;
			Car best;
			CPool<CVehicle>* pool = CPools::ms_pVehiclePool;
			if (!pool) {
				return best;
			}
			float px = 0, py = 0, pz = 0;
			S::GET_CHAR_COORDINATES(a_ped, &px, &py, &pz);
			float bestD2 = a_radius * a_radius;
			// (IV-SDK's range-for helpers for pools don't compile with clang: walk the slots.)
			for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
				CVehicle* veh = pool->Get(slot);
				if (!veh || !veh->m_pMatrix) {
					continue;
				}
				const auto& p = veh->m_pMatrix->pos;
				const float dx = p.x - px, dy = p.y - py, dz = p.z - pz;
				const float d2 = dx * dx + dy * dy + dz * dz;
				if (d2 >= bestD2 || std::fabs(dz) > kMaxDz) {
					continue;
				}
				const int handle = static_cast<int>(pool->GetIndex(veh));
				if (!handle || !S::DOES_VEHICLE_EXIST(handle) || S::IS_CAR_DEAD(handle)) {
					continue;
				}
				if (a_emptyOnly) {
					int driver = 0;
					S::GET_DRIVER_OF_CAR(handle, &driver);
					if (driver) {
						continue;
					}
				}
				bestD2 = d2;
				best.handle = handle;
			}
			best.distance = best.handle ? std::sqrt(bestD2) : 0.0f;
			return best;
		}

		void ShowMode(drive::Mode a_mode)
		{
			const char* text = a_mode == drive::Mode::kNiko ? "LibertyCraft: Niko mode (plain GTA IV)" : "LibertyCraft: Minecraft mode";
			S::PRINT_STRING_WITH_LITERAL_STRING_NOW("STRING", text, 2000, true);
		}

		void SetHidden(int a_ped, bool a_hide, const char* a_why)
		{
			if (a_hide && hiddenPed == a_ped) {
				NikoBody::Hide(a_ped, true);  // every frame: leaving puppet mode (and scripts) show him again
			} else if (a_hide && hiddenPed != a_ped) {
				if (hiddenPed) {
					NikoBody::Hide(hiddenPed, false);
				}
				NikoBody::Hide(a_ped, true);
				hiddenPed = a_ped;
				LC_LOG("Niko (ped %d) hidden: %s", a_ped, a_why);
			} else if (!a_hide && hiddenPed) {
				NikoBody::Hide(hiddenPed, false);
				hiddenPed = 0;
				LC_LOG("Niko visible again");
			}
		}

		bool gtaTookPress = false;  // GTA reacted to a tap in this attempt

		// ---- knocked over (RagdollOnVehicleHit): a car ran into the player, a blast ------------------------
		constexpr float kUprightSpeed = 0.5f;  // m/s: slower than this on his feet counts as standing
		constexpr float kHandBackSeconds = 0.5f;  // the body stays on Niko this long at most while Minecraft takes over
		// How a vehicle's knockdown is dealt (DebugVehicleHit's car-old / car-gta compare them).
		enum class KnockWay : int
		{
			kClear = 0,  // Niko is moved out of the vehicle's box first, then ragdolled and pushed (the default)
			kInPlace,    // the old way: ragdolled and pushed where he stands, inside the vehicle's box
			kGtaHit,     // moved out of its box, and the vehicle's own collision knocks him down (GTA's run-over)
		};
		const char* KnockWayName(KnockWay a_w)
		{
			return a_w == KnockWay::kInPlace ? "in place (the old way)" : a_w == KnockWay::kGtaHit ? "moved clear, GTA's own hit" : "moved clear, ragdoll and push";
		}
		struct Knock
		{
			bool     pending = false;  // ragdoll Niko once puppet mode has let go of him
			float    gx = 0.0f, gy = 0.0f, force = 0.0f, wait = 0.0f, up = 0.3f;
			int      ms = 0;
			bool     rotors = false;  // a spinning rotor is near: GTA's own would chop him as he falls
			KnockWay way = KnockWay::kClear;
			char     what[96] = "";
		};
		Knock knock;
		// Where a knockdown took the player (log): from where he was hit to where he gets up.
		struct KnockTrace
		{
			bool  active = false;
			float t = 0.0f, at[3]{}, last[3]{}, peak = 0.0f, path = 0.0f;
			float speedAt[3]{};  // his speed 0.1, 0.25 and 0.5 s after the push (m/s)
			bool  have = false;
			char  what[96] = "";
		} trace;
		KnockWay nextWay = KnockWay::kClear;  // DebugVehicleHit: how the test vehicle's knockdown is dealt
		int      nextWayVehicle = 0;
		float rotorProof = 0.0f;  // > 0: Niko is invincible this many seconds more (falling out of a rotor's reach)
		float afterKnock = 0.0f;  // > 0: watching Niko this long after a knockdown's release
		const char* roadRequest = nullptr;  // DebugRequestRoad
		int   knockdowns = 0;  // for DriveLogic, this frame
		bool  vehicleForMinecraft = false;  // GTA drives the player for a vehicle (VehicleInMinecraftMode)
		bool  dragProof = false;  // SET_CHAR_CANT_BE_DRAGGED_OUT is on for the player (seated in Minecraft mode, creative)
		bool  mcCreative = false; // the Minecraft player is in creative or spectator (kMcCreative), last known
		bool  jackSeen = false;   // a ped is trying to drag him out (logged once per attempt)
		char  uprightWhy[96] = "";  // the standing check's parts (logged when a recovery runs out of time)
		float recoverClock = 0.0f;
		// DebugBailOut / DebugRunOver
		float debugBailT = 0.0f;
		bool  debugBailed = false;
		float debugRunT = 0.0f;
		int   debugRunCar = 0;
		float debugRunDrive = 0.0f;  // seconds the test car still gets pushed at the player
		float debugRunAge = 0.0f;
		bool  debugRunRequested = false;
		// DebugCutscene
		int   debugCutPhase = 0;  // 0 waiting, 1 loading, 2 playing, 3 done
		float debugCutT = 0.0f, debugCutLog = 0.0f;

		// DebugCutscene=<name>: 20 s into puppet mode, plays one of GTA's cutscenes (INIT_CUTSCENE,
		// START_CUTSCENE, CLEAR_CUTSCENE as the mission scripts do), once. (What a mission script sets up
		// first isn't done: the intro's ship never appears, its people float over the sea.)
		void DebugCutsceneTick(const Frame& a_f)
		{
			const std::string& name = Cfg().debugCutscene;
			if (name.empty() || debugCutPhase == 3 || !a_f.exists) {
				return;
			}
			debugCutT += a_f.dt;
			switch (debugCutPhase) {
			case 0:
				if (a_f.puppeting && debugCutT >= 20.0f) {
					S::INIT_CUTSCENE(name.c_str());
					debugCutPhase = 1;
					debugCutT = 0.0f;
					LC_LOG("DebugCutscene: INIT_CUTSCENE(%s)", name.c_str());
				} else if (!a_f.puppeting) {
					debugCutT = std::min(debugCutT, 10.0f);
				}
				break;
			case 1:
				if (S::HAS_CUTSCENE_LOADED()) {
					S::START_CUTSCENE();
					debugCutPhase = 2;
					debugCutT = 0.0f;
					LC_LOG("DebugCutscene: %s loaded; START_CUTSCENE", name.c_str());
				} else if (debugCutT > 30.0f) {
					S::CLEAR_CUTSCENE();
					debugCutPhase = 3;
					LC_LOG("DebugCutscene: %s didn't load in 30 s", name.c_str());
				}
				break;
			case 2:
				if ((debugCutLog -= a_f.dt) <= 0.0f) {
					debugCutLog = 2.0f;
					const CCam* cam = TheCamera.m_pFinalCam;
					LC_LOG("DebugCutscene: %s playing, %u ms, section %u, running %d, camera %.1f %.1f %.1f", name.c_str(), S::GET_CUTSCENE_TIME(),
						S::GET_CUTSCENE_SECTION_PLAYING(), CCutsceneMgr::IsRunning() ? 1 : 0, cam ? cam->m_mMatrix.pos.x : 0.0f, cam ? cam->m_mMatrix.pos.y : 0.0f,
						cam ? cam->m_mMatrix.pos.z : 0.0f);
				}
				if (S::HAS_CUTSCENE_FINISHED() || debugCutT > 300.0f) {
					S::CLEAR_CUTSCENE();
					debugCutPhase = 3;
					LC_LOG("DebugCutscene: %s finished after %.1f s; CLEAR_CUTSCENE", name.c_str(), debugCutT);
				}
				break;
			default:
				break;
			}
		}

		void StartKnock(float a_gx, float a_gy, float a_force, int a_ms, const char* a_what, float a_up = 0.3f)
		{
			knock.pending = true;
			knock.gx = a_gx;
			knock.gy = a_gy;
			// (every knockdown's push is capped: Combat's explosions asked for up to 35, 60 m/s)
			knock.force = std::min(a_force, hit::ForceForThrow(hit::kMaxThrowSpeed));
			knock.up = a_up;
			knock.rotors = false;
			knock.ms = a_ms;
			knock.wait = 0.0f;
			knock.way = KnockWay::kClear;
			std::snprintf(knock.what, sizeof(knock.what), "%s", a_what);
			++knockdowns;
		}

		// ---- what a vehicle is to a hit (RagdollOnVehicleHit) ---------------------------------------------
		// Cars, bikes, boats and trains: their model box. Helicopters and planes: their body measured once
		// per model with line probes against its collision (VEHICLES only, this vehicle alone), as slabs
		// along its length (a cabin and a thin tail boom, not one box round both, nor round the rotor);
		// a helicopter's rotors as discs around their hubs (the vehicle structure's rotor bones).
		struct Slab
		{
			float lo[3], hi[3];
		};
		struct HitShape
		{
			hit::Kind         kind = hit::Kind::kCar;
			bool              ok = false;
			float             lo[3]{}, hi[3]{};
			std::vector<Slab> slabs;      // measured body (helicopters, planes); empty: the box
			int               slicesDone = 0, sliceHits = 0;
			bool              measured = false;  // (or not measurable: the box)
			hit::Rotor        rotor[2];   // main, tail (radius 0: none)
			int               spinBone[2] = { -1, -1 };
			float             reach = 0.0f;  // from its origin, nothing farther can touch
			bool              logged = false;
		};
		std::unordered_map<std::int32_t, HitShape> hitShapes;  // by model index
		constexpr int                              kSlices = 16;
		constexpr int                              kAcross = 13;

		struct HitTrack
		{
			hit::Pose     pose;
			std::uint32_t frame = 0;
			float         spinAxis[2][3]{};  // the moving rotor bones' y axis last frame (vehicle frame)
			bool          haveSpin[2] = { false, false };
			float         spin[2] = { 0.0f, 0.0f };  // rad/s, smoothed
		};
		std::unordered_map<int, HitTrack> hitTracks;
		std::uint32_t                     hitFrame = 0;
		struct HitCounters
		{
			std::uint32_t body = 0, rotor = 0, touchingSlow = 0;
			float         fastestTouch = 0.0f;  // m/s, touching but no hit
		} hitCounters;

		hit::Pose PoseOf(const CMatrix& a_m)
		{
			hit::Pose p;
			p.pos[0] = a_m.pos.x, p.pos[1] = a_m.pos.y, p.pos[2] = a_m.pos.z;
			p.right[0] = a_m.right.x, p.right[1] = a_m.right.y, p.right[2] = a_m.right.z;
			p.fwd[0] = a_m.up.x, p.fwd[1] = a_m.up.y, p.fwd[2] = a_m.up.z;  // IV-SDK's rows: "up" = forward, "at" = up
			p.up[0] = a_m.at.x, p.up[1] = a_m.at.y, p.up[2] = a_m.at.z;
			return p;
		}

		// A bone's matrix (CDynamicEntity::GetBoneMatrix), guarded: false if it faults.
		bool ReadBone(CVehicle* a_v, int a_bone, CMatrix& a_out)
		{
			CMatrix* m = nullptr;
			__try {
				m = a_v->GetBoneMatrix(a_bone);
				if (m) {
					a_out = *m;
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				m = nullptr;
			}
			return m != nullptr;
		}

		// The vehicle structure's bone for a part (-1: none).
		int PartBone(std::int32_t a_model, int a_part)
		{
			if (a_model < 0 || a_model >= 31000 || a_part < 0 || a_part >= 102) {
				return -1;
			}
			auto* mi = reinterpret_cast<CVehicleModelInfo*>(CModelInfo::ms_modelInfoPtrs[a_model]);
			if (!mi || !mi->m_pVehicleStruct) {
				return -1;
			}
			const std::uint32_t b = mi->m_pVehicleStruct->m_nBones[a_part];
			return b < 256 ? static_cast<int>(b) : -1;
		}

		// A bone's position in the vehicle's frame (GetBoneMatrix's may be the world's or the model's).
		bool BoneLocal(CVehicle* a_v, const hit::Pose& a_pose, int a_bone, float a_out[3], float a_axis[3] = nullptr)
		{
			CMatrix m{};
			if (a_bone < 0 || !ReadBone(a_v, a_bone, m)) {
				return false;
			}
			const float w[3] = { m.pos.x, m.pos.y, m.pos.z };
			const float dw = std::sqrt((w[0] - a_pose.pos[0]) * (w[0] - a_pose.pos[0]) + (w[1] - a_pose.pos[1]) * (w[1] - a_pose.pos[1]) +
			                           (w[2] - a_pose.pos[2]) * (w[2] - a_pose.pos[2]));
			const float dl = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
			if (!std::isfinite(dw + dl)) {
				return false;
			}
			const bool world = dw < dl;
			if (world) {
				hit::ToLocal(a_pose, w, a_out);
			} else {
				a_out[0] = w[0], a_out[1] = w[1], a_out[2] = w[2];
			}
			if (a_axis) {
				const float ax[3] = { m.up.x, m.up.y, m.up.z };  // (its y axis: turns whether it spins about z or x)
				if (world) {
					a_axis[0] = ax[0] * a_pose.right[0] + ax[1] * a_pose.right[1] + ax[2] * a_pose.right[2];
					a_axis[1] = ax[0] * a_pose.fwd[0] + ax[1] * a_pose.fwd[1] + ax[2] * a_pose.fwd[2];
					a_axis[2] = ax[0] * a_pose.up[0] + ax[1] * a_pose.up[1] + ax[2] * a_pose.up[2];
				} else {
					a_axis[0] = ax[0], a_axis[1] = ax[1], a_axis[2] = ax[2];
				}
			}
			return std::fabs(a_out[0]) < 30.0f && std::fabs(a_out[1]) < 30.0f && std::fabs(a_out[2]) < 30.0f;
		}

		// A vertical probe (the vehicle's frame) against a_veh alone: where it meets it.
		bool ProbeOwn(CVehicle* a_veh, const hit::Pose& a_pose, const float a_from[3], const float a_to[3], float a_hit[3])
		{
			float from[3], to[3];
			hit::ToWorld(a_pose, a_from, from);
			hit::ToWorld(a_pose, a_to, to);
			for (int pass = 0; pass < 3; ++pass) {
				tLineOfSightResults res;
				--col::rayCounters.rays;  // (not a map probe)
				if (!col::CastGta(from, to, res, VEHICLES)) {
					return false;
				}
				const float* p = &res.m_vEndPosition.x;
				if (!std::isfinite(p[0] + p[1] + p[2])) {
					return false;
				}
				const auto* inst = reinterpret_cast<const rage::phInst*>(res.m_pInst);
				if (!inst || inst->m_pEntity == a_veh) {
					hit::ToLocal(a_pose, p, a_hit);
					return true;
				}
				const float dir[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
				const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
				const float along = ((p[0] - from[0]) * dir[0] + (p[1] - from[1]) * dir[1] + (p[2] - from[2]) * dir[2]) / std::max(len, 1e-4f) + 0.02f;
				if (len < 1e-4f || along >= len) {
					return false;
				}
				for (int k = 0; k < 3; ++k) {
					from[k] += dir[k] / len * along;
				}
			}
			return false;
		}

		// One slice of a helicopter's or plane's body a frame: probes down and up across it (under the
		// main rotor from just below its blades: they have collision), then in from both sides for its
		// width. After the last slice a helicopter's tail rotor goes on its tail's end.
		void MeasureSlice(CVehicle* a_veh, const hit::Pose& a_pose, HitShape& a_s)
		{
			const int   i = a_s.slicesDone++;
			const float d = (a_s.hi[1] - a_s.lo[1]) / kSlices, y = a_s.lo[1] + (static_cast<float>(i) + 0.5f) * d;
			const float step = (a_s.hi[0] - a_s.lo[0]) / (kAcross - 1);
			const auto& main = a_s.rotor[0];
			Slab        slab{ { 1e9f, y - d * 0.5f, 1e9f }, { -1e9f, y + d * 0.5f, -1e9f } };
			int         hits = 0;
			for (int k = 0; k < kAcross; ++k) {
				const float x = a_s.lo[0] + step * static_cast<float>(k);
				const bool  underRotor = main.radius > 0.0f && std::hypot(x - main.hub[0], y - main.hub[1]) <= main.radius + 0.3f;
				const float topZ = underRotor ? main.hub[2] - 0.35f : a_s.hi[2] + 0.5f;
				float       h[3];
				const float top[3] = { x, y, topZ }, bottom[3] = { x, y, a_s.lo[2] - 0.5f };
				bool        any = false;
				for (int dir = 0; dir < 2; ++dir) {
					if (dir == 0 ? ProbeOwn(a_veh, a_pose, top, bottom, h) : ProbeOwn(a_veh, a_pose, bottom, top, h)) {
						slab.hi[2] = std::max(slab.hi[2], h[2]);
						slab.lo[2] = std::min(slab.lo[2], h[2]);
						any = true;
					}
				}
				if (any) {
					++hits;
					slab.lo[0] = std::min(slab.lo[0], x - step * 0.5f);
					slab.hi[0] = std::max(slab.hi[0], x + step * 0.5f);
				}
			}
			if (hits) {
				// Its width: between the outermost probe that met it and the next one out, halved 3 times.
				const auto meets = [&](float a_x) {
					const bool  underRotor = main.radius > 0.0f && std::hypot(a_x - main.hub[0], y - main.hub[1]) <= main.radius + 0.3f;
					const float top[3] = { a_x, y, underRotor ? main.hub[2] - 0.35f : a_s.hi[2] + 0.5f }, bottom[3] = { a_x, y, a_s.lo[2] - 0.5f };
					float       h[3];
					return ProbeOwn(a_veh, a_pose, top, bottom, h) || ProbeOwn(a_veh, a_pose, bottom, top, h);
				};
				for (int side = 0; side < 2; ++side) {
					float in = side == 0 ? slab.lo[0] + step * 0.5f : slab.hi[0] - step * 0.5f;  // (the outermost that met)
					float out = side == 0 ? in - step : in + step;
					for (int b = 0; b < 3; ++b) {
						const float mid = (in + out) * 0.5f;
						(meets(mid) ? in : out) = mid;
					}
					(side == 0 ? slab.lo[0] : slab.hi[0]) = (in + out) * 0.5f;
				}
				++a_s.sliceHits;
				slab.lo[0] = std::max(slab.lo[0], a_s.lo[0]), slab.hi[0] = std::min(slab.hi[0], a_s.hi[0]);
				if (slab.hi[2] - slab.lo[2] < 0.3f) {  // (a thin skin: give it some depth)
					slab.lo[2] -= 0.15f, slab.hi[2] += 0.15f;
				}
				a_s.slabs.push_back(slab);
			}
			if (a_s.slicesDone < kSlices) {
				return;
			}
			a_s.measured = true;
			if (a_s.sliceHits < kSlices / 3) {
				a_s.slabs.clear();  // the probes hardly met it: the model box
			}
			float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
			for (const auto& b : a_s.slabs) {
				for (int k = 0; k < 3; ++k) {
					lo[k] = std::min(lo[k], b.lo[k]), hi[k] = std::max(hi[k], b.hi[k]);
				}
			}
			// The tail rotor (its bone's position is its parent's, not the model's): on the side of the tail's
			// last slab, as tall as half of it.
			auto& tail = a_s.rotor[1];
			if (a_s.kind == hit::Kind::kHeli && !a_s.slabs.empty() && a_s.spinBone[1] >= 0) {
				const Slab& end = a_s.slabs.front();  // (lowest y: the tail)
				tail.hub[0] = std::fabs(tail.hub[0]) < 1.0f ? tail.hub[0] : 0.0f;
				tail.hub[1] = (end.lo[1] + end.hi[1]) * 0.5f;
				tail.hub[2] = (end.lo[2] + end.hi[2]) * 0.5f;
				tail.radius = std::clamp((end.hi[2] - end.lo[2]) * 0.5f, 0.6f, 1.2f);
				tail.axis = 0;
				tail.inner = 0.1f;
			} else {
				tail.radius = 0.0f;
			}
			LC_LOG("vehicle hits: %s model %d body measured: %d of %d slices met%s; body x %+.2f..%+.2f, y %+.2f..%+.2f, z %+.2f..%+.2f (model box x %+.2f..%+.2f, "
			       "y %+.2f..%+.2f, z %+.2f..%+.2f); tail rotor %s %+.2f %+.2f %+.2f r %.2f",
				hit::KindName(a_s.kind), a_veh->m_nModelIndex, a_s.sliceHits, kSlices, a_s.slabs.empty() ? " (too few: the model box)" : "",
				a_s.slabs.empty() ? 0.0f : lo[0], a_s.slabs.empty() ? 0.0f : hi[0], a_s.slabs.empty() ? 0.0f : lo[1], a_s.slabs.empty() ? 0.0f : hi[1],
				a_s.slabs.empty() ? 0.0f : lo[2], a_s.slabs.empty() ? 0.0f : hi[2], a_s.lo[0], a_s.hi[0], a_s.lo[1], a_s.hi[1], a_s.lo[2], a_s.hi[2],
				tail.radius > 0.0f ? "at" : "none", tail.hub[0], tail.hub[1], tail.hub[2], tail.radius);
			for (const auto& b : a_s.slabs) {
				LC_LOG("vehicle hits:   slab y %+.2f..%+.2f: x %+.2f..%+.2f, z %+.2f..%+.2f", b.lo[1], b.hi[1], b.lo[0], b.hi[0], b.lo[2], b.hi[2]);
			}
		}

		HitShape& ShapeFor(CVehicle* a_veh, int a_handle, const hit::Pose& a_pose)
		{
			auto [it, fresh] = hitShapes.try_emplace(a_veh->m_nModelIndex);
			HitShape& s = it->second;
			if (fresh) {
				s.kind = a_veh->m_nVehicleType <= 5 ? static_cast<hit::Kind>(a_veh->m_nVehicleType) : hit::Kind::kCar;
				s.ok = NpcBlocks::ModelBox(a_handle, a_veh->m_nModelIndex, s.lo, s.hi);
				s.measured = !(s.kind == hit::Kind::kHeli || s.kind == hit::Kind::kPlane);
				unsigned model = 0;
				S::GET_CAR_MODEL(a_handle, &model);
				if (s.kind == hit::Kind::kHeli) {
					// The rotors: hubs from the structure's bones (static, else moving), the main one as wide
					// as the model box's widest reach from its hub, the tail one 1.0 m.
					for (int r = 0; r < 2; ++r) {
						float hub[3];
						const int still = PartBone(a_veh->m_nModelIndex, r == 0 ? 89 : 91), moving = PartBone(a_veh->m_nModelIndex, r == 0 ? 90 : 92);
						s.spinBone[r] = moving;
						if (BoneLocal(a_veh, a_pose, still, hub) || BoneLocal(a_veh, a_pose, moving, hub)) {
							auto& ro = s.rotor[r];
							ro.hub[0] = hub[0], ro.hub[1] = hub[1], ro.hub[2] = hub[2];
							ro.axis = r == 0 ? 2 : 0;
							if (r == 0) {
								const float rx = std::max(hub[0] - s.lo[0], s.hi[0] - hub[0]);
								ro.radius = std::clamp(rx, 2.0f, 9.0f);
								ro.inner = 0.4f;
							} else {
								ro.radius = 0.0f;  // (its bone's position is its parent's: placed once the body is measured)
								ro.inner = 0.1f;
							}
						}
					}
				}
				float r2 = 0.0f;
				for (int c = 0; c < 8; ++c) {
					const float x = (c & 1) ? s.hi[0] : s.lo[0], y = (c & 2) ? s.hi[1] : s.lo[1], z = (c & 4) ? s.hi[2] : s.lo[2];
					r2 = std::max(r2, x * x + y * y + z * z);
				}
				s.reach = std::sqrt(r2) + std::max(s.rotor[0].radius, s.rotor[1].radius);
				if (s.kind != hit::Kind::kCar || !s.ok || Cfg().diagnostics) {
					LC_LOG("vehicle hits: model %08X (%s, index %d) is a %s (IS_THIS_MODEL_A_HELI %d, BIKE %d, BOAT %d); model box x %+.2f..%+.2f, "
					       "y %+.2f..%+.2f, z %+.2f..%+.2f%s",
						model, S::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(model), a_veh->m_nModelIndex, hit::KindName(s.kind), S::IS_THIS_MODEL_A_HELI(model) ? 1 : 0,
						S::IS_THIS_MODEL_A_BIKE(model) ? 1 : 0, S::IS_THIS_MODEL_A_BOAT(model) ? 1 : 0, s.lo[0], s.hi[0], s.lo[1], s.hi[1], s.lo[2], s.hi[2],
						s.ok ? "" : " (no usable box: never hits)");
				}
				if (s.kind == hit::Kind::kHeli) {
					for (int r = 0; r < 2; ++r) {
						LC_LOG("vehicle hits:   %s rotor: bones %d / %d (still / moving); %s", r == 0 ? "main" : "tail", PartBone(a_veh->m_nModelIndex, r == 0 ? 89 : 91),
							s.spinBone[r],
							s.rotor[r].radius > 0.0f || r == 1 ? "hub found" : "no hub (no rotor strikes)");
						if (s.rotor[r].radius > 0.0f) {
							LC_LOG("vehicle hits:   hub %+.2f %+.2f %+.2f (vehicle frame), radius %.2f m", s.rotor[r].hub[0], s.rotor[r].hub[1], s.rotor[r].hub[2],
								s.rotor[r].radius);
						}
					}
				}
			}
			if (!s.measured && s.ok) {
				MeasureSlice(a_veh, a_pose, s);
			}
			return s;
		}

		// How fast a rotor turns (rad/s): its moving bone's axis this frame against last frame's.
		void TrackSpin(CVehicle* a_veh, const hit::Pose& a_pose, const HitShape& a_s, HitTrack& a_t, bool a_prev, float a_dt)
		{
			for (int r = 0; r < 2; ++r) {
				float hub[3], axis[3];
				if (a_s.spinBone[r] < 0 || !BoneLocal(a_veh, a_pose, a_s.spinBone[r], hub, axis)) {
					a_t.haveSpin[r] = false;
					continue;
				}
				if (a_prev && a_t.haveSpin[r] && a_dt > 1e-4f) {
					const float* o = a_t.spinAxis[r];
					const float  dot = std::clamp(o[0] * axis[0] + o[1] * axis[1] + o[2] * axis[2], -1.0f, 1.0f);
					const float  w = std::acos(dot) / a_dt;
					if (std::isfinite(w)) {
						a_t.spin[r] += (w - a_t.spin[r]) * std::min(1.0f, a_dt * 8.0f);
					}
				}
				std::copy(axis, axis + 3, a_t.spinAxis[r]);
				a_t.haveSpin[r] = true;
			}
		}

		void Knocked(const hit::Blow& a_b, float a_gx, float a_gy, float a_up, const char* a_what, bool a_rotors = false)
		{
			StartKnock(a_gx, a_gy, a_b.force, a_b.ms, a_what, a_up);
			knock.rotors = a_rotors;
			float mcDamage = 0.0f;
			if (Config::Get().combat) {
				mcDamage = combat::McDamageFromGta(a_b.gtaDamage, Config::Get().playerDamageScale);
				Game::ReportHurt(::libertycraft::proto::kHurtOther, combat::HostDamageForMc(mcDamage), 0, 0);
			}
			LC_LOG("%s: knocked over (force %.1f, ragdoll %d ms), %.0f GTA damage -> %.2f Minecraft damage", a_what, a_b.force, a_b.ms, a_b.gtaDamage, mcDamage);
		}

		void StartTrace(const float a_at[3], const char* a_what)
		{
			trace = KnockTrace{};
			trace.active = true;
			std::copy(a_at, a_at + 3, trace.at);
			std::copy(a_at, a_at + 3, trace.last);
			std::snprintf(trace.what, sizeof(trace.what), "%s", a_what);
		}

		// Where the knockdown took him (log): his speed shortly after the push, the fastest he went and
		// where he came to rest, from where he was hit.
		void TraceTick(const Frame& a_f, bool a_done)
		{
			if (!trace.active || !a_f.exists) {
				trace.active = false;
				return;
			}
			float p[3]{};
			S::GET_CHAR_COORDINATES(a_f.ped, &p[0], &p[1], &p[2]);
			const float step = std::hypot(p[0] - trace.last[0], p[1] - trace.last[1]);
			if (a_f.dt > 1e-4f && !a_f.paused) {
				const float v = std::sqrt(step * step + (p[2] - trace.last[2]) * (p[2] - trace.last[2])) / a_f.dt;
				if (step < 5.0f) {  // (a warp isn't a throw)
					trace.peak = std::max(trace.peak, v);
					trace.path += step;
				}
				const float marks[3] = { 0.1f, 0.25f, 0.5f };
				for (int k = 0; k < 3; ++k) {
					if (trace.t < marks[k] && trace.t + a_f.dt >= marks[k]) {
						trace.speedAt[k] = v;
					}
				}
				trace.t += a_f.dt;
			}
			std::copy(p, p + 3, trace.last);
			if (a_done || trace.t > 12.0f) {
				LC_LOG("knockdown (%s): Niko came to rest %.1f m from where he was hit (%.1f m up), %.1f m along the ground, fastest %.1f m/s (%.1f, %.1f, %.1f m/s "
					   "0.1, 0.25, 0.5 s in), back up after %.1f s",
					trace.what, std::hypot(p[0] - trace.at[0], p[1] - trace.at[1]), p[2] - trace.at[2], trace.path, trace.peak, trace.speedAt[0], trace.speedAt[1],
					trace.speedAt[2], trace.t);
				trace.active = false;
			}
		}

		// A vehicle's body hit the player: before GTA's physics takes him back, he goes out of its box
		// along the push (a_ux, a_uy), clear of where it will be over the next few frames. Puppet mode
		// leaves him frozen with his collision off, so a car is already into him when the hit is seen;
		// made a ragdoll inside the car's collision, GTA's physics shot him out of it (45 to 90 m away).
		// The map in the way stops him short. Returns how far he went (m).
		float MoveClear(int a_ped, const hit::Pose& a_pose, const float a_local[3], const float a_lo[3], const float a_hi[3], float a_ux, float a_uy, float a_speed,
			float a_dt, const float a_root[3])
		{
			const float dW[3] = { a_ux, a_uy, 0.0f };
			const float dl[3] = { dW[0] * a_pose.right[0] + dW[1] * a_pose.right[1], dW[0] * a_pose.fwd[0] + dW[1] * a_pose.fwd[1],
				dW[0] * a_pose.up[0] + dW[1] * a_pose.up[1] };
			const float r = hit::Tuning{}.margin;  // the player's radius
			float       exit = 1e9f;
			for (int k = 0; k < 3; ++k) {
				if (std::fabs(dl[k]) > 1e-3f) {
					const float face = dl[k] > 0.0f ? a_hi[k] + r : a_lo[k] - r;
					exit = std::min(exit, (face - a_local[k]) / dl[k]);
				}
			}
			if (!(exit < 1e8f) || exit < 0.0f) {
				exit = 0.0f;
			}
			float dist = std::min(exit + 0.15f + a_speed * std::max(a_dt, 1.0f / 60.0f) * 3.0f, 3.0f);
			const float probe[3] = { a_root[0] + dW[0] * (dist + 0.35f), a_root[1] + dW[1] * (dist + 0.35f), a_root[2] };
			tLineOfSightResults res;
			--col::rayCounters.rays;  // (not a map harvest probe)
			if (col::CastGta(a_root, probe, res)) {
				const float* h = &res.m_vEndPosition.x;
				if (std::isfinite(h[0] + h[1] + h[2])) {
					dist = std::clamp((h[0] - a_root[0]) * dW[0] + (h[1] - a_root[1]) * dW[1] - 0.35f, 0.0f, dist);
				}
			}
			if (dist > 0.01f) {
				Game::PlacePed(a_ped, a_root[0] + dW[0] * dist, a_root[1] + dW[1] * dist, a_root[2]);
			}
			return dist;
		}

		// While puppeting: any vehicle (car, bike, boat, helicopter, plane, train) whose body runs into the
		// player (his column inside its box, or a measured slab, plus his radius) with its point there
		// moving at 3 m/s or more (helicopters, planes and trains 2), or whose spinning rotor crosses him.
		// Niko is frozen with his collision off, so GTA never sees the hit itself.
		void DetectVehicleHits(const Frame& a_f)
		{
			CPed* p = FindPlayerPed();
			auto* pool = CPools::ms_pVehiclePool;
			if (!p || !p->m_pMatrix || !pool) {
				return;
			}
			const float     root[3] = { p->m_pMatrix->pos.x, p->m_pMatrix->pos.y, p->m_pMatrix->pos.z };  // ~1 m above the feet
			const hit::Tuning tune{};
			bool            knocked = false;
			for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
				CVehicle* veh = pool->Get(slot);
				if (!veh || !veh->m_pMatrix) {
					continue;
				}
				const auto& m = *veh->m_pMatrix;
				const float dx = root[0] - m.pos.x, dy = root[1] - m.pos.y, dz = root[2] - m.pos.z, d2 = dx * dx + dy * dy + dz * dz;
				if (d2 > 60.0f * 60.0f) {
					continue;
				}
				const int       handle = static_cast<int>(pool->GetIndex(veh));
				const hit::Pose pose = PoseOf(m);
				HitShape&       s = ShapeFor(veh, handle, pose);
				auto&           tr = hitTracks[handle];
				const bool      prev = tr.frame + 1 == hitFrame;
				const hit::Pose was = tr.pose;
				tr.pose = pose;
				tr.frame = hitFrame;
				if (s.kind == hit::Kind::kHeli) {
					TrackSpin(veh, pose, s, tr, prev, a_f.dt);
				}
				if (!s.ok || !prev || knocked || std::sqrt(d2) > s.reach + 2.5f) {
					continue;
				}
				const float jump = std::hypot(pose.pos[0] - was.pos[0], pose.pos[1] - was.pos[1]) + std::fabs(pose.pos[2] - was.pos[2]);
				if (jump > 4.0f) {
					continue;  // moved by GTA (respawn, teleport)
				}
				if (!s.measured) {
					continue;  // (a helicopter's or plane's body is being measured: a frame or two)
				}
				const bool engine = veh->m_nVehicleFlags.bEngineOn;
				bool       spinning[2] = { false, false };
				for (int r = 0; r < 2 && s.kind == hit::Kind::kHeli; ++r) {
					spinning[r] = s.rotor[r].radius > 0.0f && (tr.haveSpin[r] ? tr.spin[r] > 8.0f : engine);
				}
				// ---- the body
				const float minSpeed = hit::Heavy(s.kind) ? tune.heavySpeed : tune.speed;
				hit::BodyHit best;
				const float* boxLo = s.lo;
				const float* boxHi = s.hi;
				if (s.slabs.empty()) {
					best = hit::Body(pose, was, a_f.dt, s.lo, s.hi, root, minSpeed, tune);
				} else {
					for (const auto& b : s.slabs) {
						const auto h = hit::Body(pose, was, a_f.dt, b.lo, b.hi, root, minSpeed, tune);
						if (h.hit || (h.touching && !best.touching)) {
							best = h;
							boxLo = b.lo;
							boxHi = b.hi;
							if (h.hit) {
								break;
							}
						}
					}
				}
				if (best.hit) {
					const auto  blow = hit::BodyBlow(best.speed, s.kind);
					float       gx = best.vel[0], gy = best.vel[1];
					const float h = std::hypot(gx, gy);
					if (h < 0.5f * best.speed || h < 1.0f) {
						// Mostly down (a helicopter landing on him): out from under it.
						float mid[3] = { (s.lo[0] + s.hi[0]) * 0.5f, (s.lo[1] + s.hi[1]) * 0.5f, (s.lo[2] + s.hi[2]) * 0.5f }, midW[3];
						hit::ToWorld(pose, mid, midW);
						gx = root[0] - midW[0], gy = root[1] - midW[1];
					}
					const float g = std::max(std::hypot(gx, gy), 1e-3f);
					char        what[128];
					std::snprintf(what, sizeof(what), "%s %d hit the player at %.1f m/s (%.1f down)", hit::KindName(s.kind), handle, best.speed, -best.vel[2]);
					LC_LOG("%s: the player at %+.2f %+.2f %+.2f in its frame (%s)", what, best.local[0], best.local[1], best.local[2],
						s.slabs.empty() ? "model box" : "measured body");
					const KnockWay way = handle == nextWayVehicle ? nextWay : KnockWay::kClear;
					float          moved = 0.0f;
					if (way != KnockWay::kInPlace) {
						moved = MoveClear(a_f.ped, pose, best.local, boxLo, boxHi, gx / g, gy / g, best.speed, a_f.dt, root);
					}
					Knocked(blow, gx / g, gy / g, hit::kBodyUp, what, spinning[0] || spinning[1]);
					knock.way = way;
					if (way != KnockWay::kClear || moved > 0.0f) {
						LC_LOG("%s: %s (moved %.2f m along the push first)", what, KnockWayName(way), moved);
					}
					StartTrace(root, what);
					++hitCounters.body;
					knocked = true;
					continue;
				}
				if (best.touching) {
					++hitCounters.touchingSlow;
					hitCounters.fastestTouch = std::max(hitCounters.fastestTouch, best.speed);
				}
				// ---- a helicopter's spinning rotors
				if (s.kind != hit::Kind::kHeli) {
					continue;
				}
				for (int r = 0; r < 2 && !knocked; ++r) {
					if (!spinning[r]) {
						continue;
					}
					const auto st = hit::Strike(pose, s.rotor[r], root, tune);
					if (!st.hit) {
						continue;
					}
					char what[128];
					std::snprintf(what, sizeof(what), "the %s rotor of helicopter %d (%.0f rad/s%s) struck the player %.1f m from its hub", r == 0 ? "main" : "tail", handle,
						tr.spin[r], tr.haveSpin[r] ? "" : ", engine on", st.rho);
					Knocked(hit::RotorBlow(r == 0), st.fling[0], st.fling[1], std::max(0.3f, st.fling[2] * 1.5f), what, true);
					StartTrace(root, what);
					++hitCounters.rotor;
					knocked = true;
				}
			}
			for (auto it = hitTracks.begin(); it != hitTracks.end();) {
				it = it->second.frame == hitFrame ? std::next(it) : hitTracks.erase(it);
			}
		}

		// DebugBailOut: DebugAutoVehicle's car drives off at speed after 3 s and Niko bails out at 5 s.
		bool DebugBailOutTick(const Frame& a_f)
		{
			if (!Cfg().debugBailOut || !a_f.inCar) {
				debugBailT = 0.0f;
				debugBailed = false;
				return false;
			}
			debugBailT += a_f.dt;
			int veh = 0;
			S::GET_CAR_CHAR_IS_USING(a_f.ped, &veh);
			if (veh && S::DOES_VEHICLE_EXIST(veh) && debugBailT >= 3.0f && debugBailT < 6.0f) {
				S::SET_CAR_FORWARD_SPEED(veh, 14.0f);
			}
			if (!debugBailed && debugBailT >= 5.0f) {
				debugBailed = true;
				float speed = 0.0f;
				if (veh) {
					S::GET_CAR_SPEED(veh, &speed);
				}
				LC_LOG("DebugBailOut: GTA's exit control at %.1f m/s", speed);
				return true;
			}
			return false;
		}

		bool StartRelocation(int a_ped, const char* a_who, int a_interior, float a_aboveGround);

		// DebugRunOver: every 40 s of puppeting (outdoors), a test car 15 m up the road drives at the player.
		void DebugRunOverTick(const Frame& a_f)
		{
			if (!Cfg().debugRunOver || !a_f.exists) {
				return;
			}
			if (debugRunCar) {
				debugRunAge += a_f.dt;
				const bool exists = S::DOES_VEHICLE_EXIST(debugRunCar);
				if (exists && debugRunDrive > 0.0f && !knock.pending && !logic.recovering()) {
					debugRunDrive -= a_f.dt;
					S::SET_CAR_FORWARD_SPEED(debugRunCar, 12.0f);
				}
				if (!exists || debugRunAge > 12.0f) {
					if (exists) {
						S::MARK_CAR_AS_NO_LONGER_NEEDED(&debugRunCar);
					}
					debugRunCar = 0;
				}
			}
			if (!a_f.puppeting || a_f.paused || logic.mode() != drive::Mode::kMinecraft) {
				return;
			}
			debugRunT += a_f.dt;
			if (debugRunT < 15.0f || debugRunCar) {
				return;
			}
			float aboveGround = 99.0f;
			S::GET_CHAR_HEIGHT_ABOVE_GROUND(a_f.ped, &aboveGround);
			int interior = 0;
			S::GET_INTERIOR_FROM_CHAR(a_f.ped, &interior);
			if (aboveGround > 2.0f || interior != 0) {
				if (debugRunT > 21.0f && StartRelocation(a_f.ped, "DebugRunOver", interior, aboveGround)) {
					debugRunT = 0.0f;
				}
				return;
			}
			const unsigned int model = S::GET_HASH_KEY("admiral");
			if (!debugRunRequested) {
				debugRunRequested = true;
				CStreaming::ScriptRequestModel(static_cast<std::int32_t>(model));
			}
			if (!S::HAS_MODEL_LOADED(model)) {
				return;
			}
			debugRunRequested = false;
			float x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0, heading = 0;
			S::GET_CHAR_COORDINATES(a_f.ped, &x, &y, &z);
			if (!S::GET_CLOSEST_CAR_NODE_WITH_HEADING(x, y, z, &nx, &ny, &nz, &heading)) {
				heading = 0.0f;
			}
			const float h = heading * kDegToRad;
			const float sx = x - std::sin(h) * 15.0f, sy = y + std::cos(h) * 15.0f;
			float       ground = z;
			S::GET_GROUND_Z_FOR_3D_COORD(sx, sy, z + 3.0f, &ground);
			const float aim = std::atan2(-(x - sx), y - sy) / kDegToRad;
			S::CREATE_CAR(model, sx, sy, ground + 0.5f, &debugRunCar, true);
			S::MARK_MODEL_AS_NO_LONGER_NEEDED(model);
			if (debugRunCar) {
				S::SET_CAR_HEADING(debugRunCar, aim);
			}
			debugRunDrive = 3.0f;
			debugRunAge = 0.0f;
			debugRunT = -25.0f;  // the next one 40 s on
			LC_LOG("DebugRunOver: test car %d 15 m up the road (%.1f %.1f %.1f), driving at the player at 12 m/s (heading %.0f)", debugRunCar, sx, sy, ground,
				aim);
		}

		// DebugVehicleHit=heli,drop,rotor,bike,car: one test vehicle at a time runs into the puppeted
		// player, 20 s into puppet mode and 15 s after the last one is gone (back on his feet):
		//  - heli: a Maverick (engine on, blades at full speed) 16 m ahead of the camera, a little off the
		//    ground, flies at him at 8 m/s (its velocity held level every frame);
		//  - drop: one 9 m over him comes down at 4 m/s;
		//  - rotor: one 14 m ahead on the ground creeps at him at 3 m/s until its hub is 2.5 m away (stand
		//    the player on something about 3 m up first: its rotor reaches him before its body);
		//  - bike: a PCJ 15 m ahead of the camera drives at him at 12 m/s;
		//  - car: an Admiral 15 m ahead of the camera drives at him at 10 m/s (car-old: his knockdown dealt
		//    the old way, in place; car-gta: moved clear, then the car's own collision knocks him over);
		//  - ped: GTA's own run-over to compare with: a pedestrian stands still 9 m ahead of the camera (3 m
		//    to the right) and an Admiral comes at it from 15 m beyond it at 10 m/s; where it lands is logged.
		// The cars drive on unpushed once they touch him (or the pedestrian). The vehicle is deleted 8 s
		// after it started (the pedestrian's car 10 s).
		constexpr float kTestCarSpeed = 10.0f;
		struct VehicleHitTest
		{
			std::vector<std::string> list;
			bool                     parsed = false;
			std::size_t              next = 0;
			float                    wait = 30.0f;
			int                      car = 0;
			unsigned                 model = 0;
			std::string              kind;
			float                    age = 0.0f, logT = 0.0f;
			bool                     requested = false, pushing = false;
			// ped: the pedestrian, where it stood, where the car touched it, its path from there
			int                      ped = 0;
			float                    pedAt[3]{};
			bool                     contact = false;
			float                    contactT = 0.0f, contactAt[3]{}, last[3]{}, peak = 0.0f, path = 0.0f, speedAt[3]{}, carSpeed = 0.0f;
			bool                     logged = false;
		} vht;

		void PushVehicle(int a_car, const CVector& a_v)
		{
			CVehicle* v = CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->GetAt(static_cast<std::uint32_t>(a_car)) : nullptr;
			CVector   vel = a_v;
			if (v) {
				if (auto* c = v->GetConstrainedCollider()) {
					c->SetVelocity(&vel);
					return;
				}
			}
			S::SET_CAR_FORWARD_SPEED(a_car, std::hypot(a_v.x, a_v.y));
		}

		// DebugVehicleHit's pedestrian: the car touching it (its model box and the pedestrian's root, as the
		// player's hits are found), then its path from there.
		void PedTestTick(VehicleHitTest& a_t, CVehicle* a_v, const Frame& a_f)
		{
			CPed* ped = a_t.ped && CPools::ms_pPedPool ? CPools::ms_pPedPool->GetAt(static_cast<std::uint32_t>(a_t.ped)) : nullptr;
			if (!ped || !ped->m_pMatrix || !S::DOES_CHAR_EXIST(a_t.ped)) {
				return;
			}
			const float root[3] = { ped->m_pMatrix->pos.x, ped->m_pMatrix->pos.y, ped->m_pMatrix->pos.z };
			if (!a_t.contact && a_v && a_v->m_pMatrix) {
				const hit::Pose pose = PoseOf(*a_v->m_pMatrix);
				const HitShape& s = ShapeFor(a_v, a_t.car, pose);
				if (s.ok && hit::Body(pose, pose, 0.0f, s.lo, s.hi, root, 0.0f).touching) {
					a_t.contact = true;
					a_t.pushing = false;
					a_t.contactT = a_t.age;
					std::copy(root, root + 3, a_t.contactAt);
					std::copy(root, root + 3, a_t.last);
					S::GET_CAR_SPEED(a_t.car, &a_t.carSpeed);
					LC_LOG("DebugVehicleHit: ped test: the car touched pedestrian %d at %.1f m/s (%.2f m from where it stood)", a_t.ped, a_t.carSpeed,
						std::hypot(root[0] - a_t.pedAt[0], root[1] - a_t.pedAt[1]));
				}
			}
			if (!a_t.contact || a_t.logged) {
				return;
			}
			const float since = a_t.age - a_t.contactT;
			const float step = std::hypot(root[0] - a_t.last[0], root[1] - a_t.last[1]);
			if (a_f.dt > 1e-4f) {
				const float v = std::sqrt(step * step + (root[2] - a_t.last[2]) * (root[2] - a_t.last[2])) / a_f.dt;
				a_t.peak = std::max(a_t.peak, v);
				const float marks[3] = { 0.1f, 0.25f, 0.5f };
				for (int k = 0; k < 3; ++k) {
					if (since - a_f.dt < marks[k] && since >= marks[k]) {
						a_t.speedAt[k] = v;
					}
				}
			}
			a_t.path += step;
			std::copy(root, root + 3, a_t.last);
			if (since >= 6.0f) {
				a_t.logged = true;
				LC_LOG("DebugVehicleHit: ped test: GTA's run-over at %.1f m/s left pedestrian %d %.1f m from where the car touched it (%.1f m up), %.1f m along the "
					   "ground, fastest %.1f m/s (%.1f, %.1f, %.1f m/s 0.1, 0.25, 0.5 s in), ragdoll %d, health %u",
					a_t.carSpeed, a_t.ped, std::hypot(root[0] - a_t.contactAt[0], root[1] - a_t.contactAt[1]), root[2] - a_t.contactAt[2], a_t.path, a_t.peak,
					a_t.speedAt[0], a_t.speedAt[1], a_t.speedAt[2], S::IS_PED_RAGDOLL(a_t.ped) ? 1 : 0, [&] {
						unsigned h = 0;
						S::GET_CHAR_HEALTH(a_t.ped, &h);
						return h;
					}());
			}
		}

		void DebugVehicleHitTick(const Frame& a_f)
		{
			auto& t = vht;
			if (Cfg().debugVehicleHit.empty() || !a_f.exists) {
				return;
			}
			if (!t.parsed) {
				t.parsed = true;
				std::string cur;
				for (const char c : Cfg().debugVehicleHit + ",") {
					if (c == ',') {
						if (!cur.empty()) {
							t.list.push_back(cur);
						}
						cur.clear();
					} else if (c != ' ') {
						cur += c;
					}
				}
			}
			CPed* player = FindPlayerPed();
			if (t.car) {
				t.age += a_f.dt;
				const bool exists = S::DOES_VEHICLE_EXIST(t.car);
				CVehicle*  v = exists && CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->GetAt(static_cast<std::uint32_t>(t.car)) : nullptr;
				const bool pedTest = t.kind == "ped";
				const bool down = !pedTest && (knock.pending || logic.recovering());
				if (pedTest) {
					PedTestTick(t, v, a_f);
				}
				if (v && v->m_pMatrix && player && player->m_pMatrix && t.pushing && !down && t.age < 6.0f) {
					const auto& pos = v->m_pMatrix->pos;
					const auto& me = player->m_pMatrix->pos;
					const float tx = pedTest ? t.pedAt[0] : me.x, ty = pedTest ? t.pedAt[1] : me.y;
					const float dx = tx - pos.x, dy = ty - pos.y, d = std::max(std::hypot(dx, dy), 0.01f);
					if (t.kind == "heli") {
						PushVehicle(t.car, CVector{ dx / d * 8.0f, dy / d * 8.0f, 0.0f });
					} else if (t.kind == "drop") {
						PushVehicle(t.car, CVector{ dx / d * std::min(d * 2.0f, 1.0f), dy / d * std::min(d * 2.0f, 1.0f), -4.0f });
					} else if (t.kind == "rotor") {
						if (d < 2.5f) {
							t.pushing = false;
							PushVehicle(t.car, CVector{ 0.0f, 0.0f, 0.0f });
						} else {
							PushVehicle(t.car, CVector{ dx / d * 3.0f, dy / d * 3.0f, 0.0f });
						}
					} else {
						// (riderless bikes wander off a straight line: aimed at him every frame)
						const float speed = t.kind == "bike" ? 12.0f : kTestCarSpeed;
						CVector     vel{};
						v->GetVelocity(&vel);
						S::SET_CAR_HEADING(t.car, std::atan2(-dx, dy) / kDegToRad);
						PushVehicle(t.car, CVector{ dx / d * speed, dy / d * speed, vel.z });
					}
					if (t.kind == "heli" || t.kind == "drop" || t.kind == "rotor") {
						S::SET_HELI_BLADES_FULL_SPEED(t.car);
					}
				} else if (down) {
					t.pushing = false;
				}
				if (v && v->m_pMatrix && player && player->m_pMatrix && (t.logT -= a_f.dt) <= 0.0f) {
					t.logT = 0.25f;
					CVector vel{};
					v->GetVelocity(&vel);
					const auto& pos = v->m_pMatrix->pos;
					const auto& me = player->m_pMatrix->pos;
					const auto  it = hitTracks.find(t.car);
					char        pedText[96] = "";
					if (pedTest && t.ped && S::DOES_CHAR_EXIST(t.ped)) {
						float px = 0, py = 0, pz = 0;
						S::GET_CHAR_COORDINATES(t.ped, &px, &py, &pz);
						std::snprintf(pedText, sizeof(pedText), "; pedestrian %.1f m from where it stood%s", std::hypot(px - t.pedAt[0], py - t.pedAt[1]),
							S::IS_PED_RAGDOLL(t.ped) ? ", ragdoll" : "");
					}
					LC_LOG("DebugVehicleHit: %s %d at %.1f s: %.1f m from the player (%.1f up), velocity %.1f %.1f %.1f, engine %s, rotor %.0f / %.0f rad/s%s",
						t.kind.c_str(), t.car, t.age, std::hypot(me.x - pos.x, me.y - pos.y), pos.z - me.z, vel.x, vel.y, vel.z,
						v->m_nVehicleFlags.bEngineOn ? "on" : "off", it != hitTracks.end() ? it->second.spin[0] : -1.0f,
						it != hitTracks.end() ? it->second.spin[1] : -1.0f, pedText);
				}
				if (!exists || t.age > (pedTest ? 10.0f : 8.0f)) {
					if (exists) {
						S::DELETE_CAR(&t.car);
					}
					if (t.ped && S::DOES_CHAR_EXIST(t.ped)) {
						S::DELETE_CHAR(&t.ped);
					}
					LC_LOG("DebugVehicleHit: %s test over%s", t.kind.c_str(), pedTest && !t.contact ? " (the car never touched the pedestrian)" : "");
					t.car = 0;
					t.ped = 0;
					t.wait = 15.0f;
					nextWayVehicle = 0;
				}
				return;
			}
			if (!a_f.puppeting || a_f.paused || logic.mode() != drive::Mode::kMinecraft || logic.recovering() || t.next >= t.list.size() || !player ||
				!player->m_pMatrix) {
				return;
			}
			if ((t.wait -= a_f.dt) > 0.0f) {
				return;
			}
			const std::string& kind = t.list[t.next];
			const bool         heli = kind == "heli" || kind == "drop" || kind == "rotor";
			const unsigned     model = S::GET_HASH_KEY(heli ? "maverick" : kind == "bike" ? "pcj" : "admiral");
			if (!t.requested) {
				t.requested = true;
				CStreaming::ScriptRequestModel(static_cast<std::int32_t>(model));
			}
			if (!S::HAS_MODEL_LOADED(model)) {
				return;
			}
			t.requested = false;
			++t.next;
			const auto& me = player->m_pMatrix->pos;
			float       fx = 0.0f, fy = 1.0f;
			if (const CCam* cam = TheCamera.m_pFinalCam) {
				const float h = std::hypot(cam->m_mMatrix.up.x, cam->m_mMatrix.up.y);
				if (h > 0.1f) {
					fx = cam->m_mMatrix.up.x / h, fy = cam->m_mMatrix.up.y / h;
				}
			}
			const bool  pedTest = kind == "ped";
			const float dist = kind == "drop" ? 0.0f : kind == "rotor" ? 14.0f : kind == "heli" ? 16.0f : 15.0f;
			// The pedestrian stands 9 m ahead of the camera and 3 m to its right; its car starts 15 m beyond
			// it and comes back toward the camera (along the street the camera looks down, past the player).
			const float pedX = me.x + fx * 9.0f + fy * 3.0f, pedY = me.y + fy * 9.0f - fx * 3.0f;
			const float x = pedTest ? pedX + fx * 15.0f : me.x + fx * dist, y = pedTest ? pedY + fy * 15.0f : me.y + fy * dist;
			float       ground = me.z - 1.0f, here = me.z - 1.0f;
			S::GET_GROUND_Z_FOR_3D_COORD(x, y, me.z + 3.0f, &ground);
			S::GET_GROUND_Z_FOR_3D_COORD(me.x, me.y, me.z + 1.0f, &here);  // (GTA's ground under him: not Minecraft's blocks)
			if (!std::isfinite(here) || here < me.z - 8.0f || here > me.z + 1.0f) {
				here = me.z - 1.0f;
			}
			if (!std::isfinite(ground) || ground < here - 3.0f || ground > me.z + 2.0f) {
				ground = here;  // (over water: the sea bed)
			}
			if (heli) {
				ground = std::max(ground, here);
			}
			if (pedTest) {
				float pedGround = here;
				S::GET_GROUND_Z_FOR_3D_COORD(pedX, pedY, me.z + 3.0f, &pedGround);
				if (!std::isfinite(pedGround) || pedGround < here - 3.0f || pedGround > me.z + 2.0f) {
					pedGround = here;
				}
				t.ped = 0;
				S::CREATE_RANDOM_CHAR(pedX, pedY, pedGround + 1.0f, &t.ped);
				if (!t.ped) {
					LC_LOG("DebugVehicleHit: CREATE_RANDOM_CHAR failed for the ped test");
					t.wait = 5.0f;
					return;
				}
				S::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(t.ped, true);
				S::SET_PED_DONT_DO_EVASIVE_DIVES(t.ped, true);
				S::SET_CHAR_HEADING(t.ped, std::atan2(-(me.x - pedX), me.y - pedY) / kDegToRad);
				S::TASK_STAND_STILL(t.ped, 30000);
				t.pedAt[0] = pedX, t.pedAt[1] = pedY, t.pedAt[2] = pedGround + 1.0f;
				t.contact = t.logged = false;
				t.peak = t.path = 0.0f;
				std::fill(t.speedAt, t.speedAt + 3, 0.0f);
			}
			const float z = kind == "drop" ? me.z + 9.0f : kind == "heli" ? ground + 1.2f : ground + 0.5f;
			S::CREATE_CAR(model, x, y, z, &t.car, true);
			S::MARK_MODEL_AS_NO_LONGER_NEEDED(model);
			if (!t.car) {
				LC_LOG("DebugVehicleHit: CREATE_CAR failed for %s", kind.c_str());
				if (t.ped) {
					S::DELETE_CHAR(&t.ped);
				}
				t.wait = 5.0f;
				return;
			}
			const float tx = pedTest ? pedX : me.x, ty = pedTest ? pedY : me.y;
			const float aim = std::atan2(-(tx - x), ty - y) / kDegToRad;
			S::SET_CAR_HEADING(t.car, kind == "drop" ? std::atan2(-fx, fy) / kDegToRad : aim);
			if (heli) {
				S::SET_CAR_ENGINE_ON(t.car, true, true);
				S::SET_HELI_BLADES_FULL_SPEED(t.car);
			}
			nextWay = kind == "car-old" ? KnockWay::kInPlace : kind == "car-gta" ? KnockWay::kGtaHit : KnockWay::kClear;
			nextWayVehicle = t.car;
			t.kind = kind;
			t.model = model;
			t.age = 0.0f;
			t.logT = 0.0f;
			t.pushing = true;
			LC_LOG("DebugVehicleHit: %s test: vehicle %d at %.1f %.1f %.1f (%.1f m from the player, ground %.1f), heading %.0f%s", kind.c_str(), t.car, x, y, z,
				std::hypot(x - me.x, y - me.y), ground, aim, pedTest ? ", at a pedestrian 9 m ahead" : "");
		}

		void EnterByOtherMeans(int a_ped)
		{
			const auto& how = Cfg().vehicleEnterFallback;
			if (how == "none") {
				LC_LOG("the enter press didn't take; VehicleEnterFallback=none");
				return;
			}
			// A warp needs a free driver's seat; the task pulls the driver out like GTA's own F.
			const Car car = ClosestCar(a_ped, kFallbackRadius, how != "task");
			if (gtaTookPress) {
				LC_LOG("GTA took the enter press but Niko stopped short of getting in");
			} else {
				LC_LOG("the enter press didn't take (GTA's enter control read %d, last %d before our press)", padEnterCurrent.load(), padEnterLast.load());
			}
			if (!car.handle) {
				LC_LOG("no %svehicle is within %.0f m", how != "task" ? "empty " : "", kFallbackRadius);
				return;
			}
			if (how == "task") {
				LC_LOG("VehicleEnterFallback: TASK_ENTER_CAR_AS_DRIVER vehicle %d (%.1f m)", car.handle, car.distance);
				S::TASK_ENTER_CAR_AS_DRIVER(a_ped, car.handle, 10000);
			} else {
				LC_LOG("VehicleEnterFallback: WARP_CHAR_INTO_CAR vehicle %d (%.1f m)", car.handle, car.distance);
				S::WARP_CHAR_INTO_CAR(a_ped, car.handle);
			}
		}

		// Test hooks: a save can start indoors (Roman's flat); move Niko out to the nearest road (puppet
		// mode off for a few frames, then the usual resync).
		bool StartRelocation(int a_ped, const char* a_who, int a_interior, float a_aboveGround)
		{
			float x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0;
			S::GET_CHAR_COORDINATES(a_ped, &x, &y, &z);
			if (debugRelocate || !S::GET_CLOSEST_CAR_NODE(x, y, z, &nx, &ny, &nz)) {
				return false;
			}
			debugRelocate = 3;
			debugRelocateTo[0] = nx, debugRelocateTo[1] = ny, debugRelocateTo[2] = nz;  // (SET_CHAR_COORDINATES adds 1 m: the root)
			LC_LOG("%s: indoors/above ground (interior %d, %.1f m up): moving Niko to the road at %.1f %.1f %.1f", a_who, a_interior, a_aboveGround, nx, ny, nz);
			return true;
		}

		std::uint32_t PoolUsed(bool a_vehicles)
		{
			if (a_vehicles) {
				return CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->m_nUsed : 0;
			}
			return CPools::ms_pPedPool ? CPools::ms_pPedPool->m_nUsed : 0;
		}

		// The ped within 8 m of a_ped that is jacking a vehicle (IS_PED_JACKING), 0 if none.
		int FindJacker(int a_ped)
		{
			CPool<CPed>* pool = CPools::ms_pPedPool;
			if (!pool) {
				return 0;
			}
			float px = 0, py = 0, pz = 0;
			S::GET_CHAR_COORDINATES(a_ped, &px, &py, &pz);
			for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
				CPed* p = pool->Get(slot);
				if (!p || !p->m_pMatrix) {
					continue;
				}
				const auto& m = p->m_pMatrix->pos;
				const float dx = m.x - px, dy = m.y - py;
				const int   handle = static_cast<int>(pool->GetIndex(p));
				if (dx * dx + dy * dy < 64.0f && handle && handle != a_ped && S::DOES_CHAR_EXIST(handle) && S::IS_PED_JACKING(handle)) {
					return handle;
				}
			}
			return 0;
		}

		// The closest living pedestrian on foot within a_radius, other than a_ped (the ped pool).
		int ClosestPed(int a_ped, float a_radius)
		{
			CPool<CPed>* pool = CPools::ms_pPedPool;
			if (!pool) {
				return 0;
			}
			float px = 0, py = 0, pz = 0;
			S::GET_CHAR_COORDINATES(a_ped, &px, &py, &pz);
			float bestD2 = a_radius * a_radius;
			int   best = 0;
			for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
				CPed* p = pool->Get(slot);
				if (!p || !p->m_pMatrix) {
					continue;
				}
				const auto& m = p->m_pMatrix->pos;
				const float dx = m.x - px, dy = m.y - py, dz = m.z - pz, d2 = dx * dx + dy * dy;
				if (d2 >= bestD2 || std::fabs(dz) > 3.0f) {
					continue;
				}
				const int handle = static_cast<int>(pool->GetIndex(p));
				if (!handle || handle == a_ped || !S::DOES_CHAR_EXIST(handle) || S::IS_CHAR_DEAD(handle) || S::IS_CHAR_IN_ANY_CAR(handle)) {
					continue;
				}
				bestD2 = d2;
				best = handle;
			}
			return best;
		}

		void WalkTestTick(const Frame& a_f)
		{
			auto& w = walk;
			w.dt = a_f.dt;
			if (!Cfg().debugWalkThroughCar || w.round >= 2 || a_f.paused) {
				return;
			}
			if (!a_f.puppeting || a_f.inCar || a_f.dead) {
				w.wait = 0.0f;
				if (w.step >= 3) {
					LC_LOG("DebugWalkThroughCar: puppet mode ended mid-walk; starting this round again");
					w.step = 0;
					w.active = false;
				}
				return;
			}
			w.t += a_f.dt;
			switch (w.step) {
			case 0: {
				if ((w.wait += a_f.dt) < (w.round == 0 ? 10.0f : 4.0f)) {
					break;
				}
				float aboveGround = 99.0f;
				S::GET_CHAR_HEIGHT_ABOVE_GROUND(a_f.ped, &aboveGround);
				int interior = 0;
				S::GET_INTERIOR_FROM_CHAR(a_f.ped, &interior);
				if (aboveGround > 2.0f || interior != 0) {
					StartRelocation(a_f.ped, "DebugWalkThroughCar", interior, aboveGround);
					w.wait = 0.0f;
					break;
				}
				w.step = 1;
				w.modelRequested = false;
				break;
			}
			case 1: {
				const unsigned int model = S::GET_HASH_KEY("admiral");
				if (!w.modelRequested) {
					w.modelRequested = true;
					CStreaming::ScriptRequestModel(static_cast<std::int32_t>(model));
				}
				if (!S::HAS_MODEL_LOADED(model)) {
					break;
				}
				float x = 0, y = 0, z = 0, h = 0;
				S::GET_CHAR_COORDINATES(a_f.ped, &x, &y, &z);
				S::GET_CHAR_HEADING(a_f.ped, &h);
				w.dir[0] = -std::sin(h * kDegToRad);
				w.dir[1] = std::cos(h * kDegToRad);
				w.carPos[0] = x + w.dir[0] * 5.0f, w.carPos[1] = y + w.dir[1] * 5.0f, w.carPos[2] = z;
				w.car = 0;
				S::CREATE_CAR(model, w.carPos[0], w.carPos[1], w.carPos[2], &w.car, true);
				S::MARK_MODEL_AS_NO_LONGER_NEEDED(model);
				if (w.car) {
					S::SET_CAR_HEADING(w.car, h + 90.0f);  // across the path
				}
				w.ped = 0;
				S::CREATE_RANDOM_CHAR(x + w.dir[0] * 9.0f, y + w.dir[1] * 9.0f, z, &w.ped);
				if (w.ped) {
					S::TASK_STAND_STILL(w.ped, 20000);
				}
				// Ambient now, like the cars and pedestrians on the street (the clearing spares mission entities).
				int car = w.car, ped = w.ped;
				if (car) {
					S::MARK_CAR_AS_NO_LONGER_NEEDED(&car);
				}
				if (ped) {
					S::MARK_CHAR_AS_NO_LONGER_NEEDED(&ped);
				}
				LC_LOG("DebugWalkThroughCar round %d (%s move): car %d parked 5 m ahead across the path at %.1f %.1f %.1f, pedestrian %d 9 m ahead",
					w.round + 1, w.round == 0 ? "native" : "direct", w.car, w.carPos[0], w.carPos[1], w.carPos[2], w.ped);
				w.step = 2;
				w.t = 0.0f;
				break;
			}
			case 2:
				if (w.t >= 1.5f) {
					w.vehBefore = PoolUsed(true);
					w.pedsBefore = PoolUsed(false);
					w.carGone = w.pedGone = false;
					w.needStart = true;
					w.active = true;
					w.step = 3;
					w.t = 0.0f;
				}
				break;
			default: {
				if (w.car && !w.carGone && !S::DOES_VEHICLE_EXIST(w.car)) {
					w.carGone = true;
					w.carGoneAt = w.t;
					const float dx = static_cast<float>(w.target.x) - w.carPos[0], dy = static_cast<float>(w.target.y) - w.carPos[1];
					LC_LOG("DebugWalkThroughCar round %d: car %d DELETED (%.2f s into step %d, the player %.1f m from where it was parked)", w.round + 1, w.car,
						w.t, w.step, std::sqrt(dx * dx + dy * dy));
				}
				if (w.ped && !w.pedGone && !S::DOES_CHAR_EXIST(w.ped)) {
					w.pedGone = true;
					w.pedGoneAt = w.t;
					LC_LOG("DebugWalkThroughCar round %d: pedestrian %d DELETED (%.2f s into step %d)", w.round + 1, w.ped, w.t, w.step);
				}
				{
					const float dx = static_cast<float>(w.target.x) - w.carPos[0], dy = static_cast<float>(w.target.y) - w.carPos[1];
					w.carClosest = std::min(w.carClosest, std::sqrt(dx * dx + dy * dy));
				}
				if (w.step == 4 && w.ped && !w.pedGone && S::DOES_CHAR_EXIST(w.ped)) {
					float x = 0, y = 0, z = 0;
					S::GET_CHAR_COORDINATES(w.ped, &x, &y, &z);
					const float dx = static_cast<float>(w.target.x) - x, dy = static_cast<float>(w.target.y) - y;
					w.pedClosest = std::min(w.pedClosest, std::sqrt(dx * dx + dy * dy));
				}
				if (w.step == 3 && w.t >= 6.0f) {
					// Into the closest pedestrian on foot (an ambient one if one is nearer than ours).
					w.ped = ClosestPed(a_f.ped, 30.0f);
					w.step = w.ped ? 4 : 5;
					w.t = 0.0f;
					if (w.ped) {
						LC_LOG("DebugWalkThroughCar round %d: walking into pedestrian %d", w.round + 1, w.ped);
					}
				} else if (w.step == 4 && w.t >= 5.0f) {
					w.step = 5;
					w.t = 0.0f;
				} else if (w.step == 5 && w.t >= 2.0f) {
					w.active = false;
					LC_LOG("DebugWalkThroughCar round %d (%s move): car %d %s (the player came within %.2f m of its centre), pedestrian %d %s (within %.2f m); "
						   "vehicle pool %u -> %u, ped pool %u -> %u",
						w.round + 1, w.round == 0 ? "native" : "direct", w.car, w.carGone ? "DELETED" : "still there", w.carClosest, w.ped,
						!w.ped ? "(none)" : w.pedGone ? "DELETED" : "still there", w.pedClosest, w.vehBefore, PoolUsed(true), w.pedsBefore, PoolUsed(false));
					w.carClosest = w.pedClosest = 99.0f;
					++w.round;
					w.step = 0;
					w.wait = 0.0f;
				}
				break;
			}
			}
		}

		bool Usable()
		{
			const auto& st = Game::State();
			return !st.gtaMenuOpen.load(std::memory_order_relaxed);
		}
	}

	bool OnKey(std::uint32_t a_dik, bool a_down, bool a_repeat)
	{
		if (!a_dik || !Usable()) {
			return false;
		}
		const auto& st = Game::State();
		const bool  puppeting = st.puppeting.load(std::memory_order_relaxed);
		if (puppeting && st.mcScreenOpen.load(std::memory_order_relaxed)) {
			return false;  // typing into a Minecraft screen
		}
		const auto& cfg = Cfg();
		if (a_dik == cfg.ToggleKeyDik()) {
			if (a_down && !a_repeat) {
				togglePresses.fetch_add(1, std::memory_order_relaxed);
			}
			return true;
		}
		if (puppeting && a_dik == cfg.VehicleKeyDik()) {
			if (a_down && !a_repeat) {
				vehiclePresses.fetch_add(1, std::memory_order_relaxed);
			}
			return true;  // Minecraft never sees it (its F would swap hands)
		}
		return false;
	}

	void Pad(CPad* a_pad)
	{
		if (!a_pad) {
			return;
		}
		padFrames.fetch_add(1, std::memory_order_relaxed);
		if (pressEnter.load(std::memory_order_relaxed)) {
			padEnterCurrent.store(a_pad->m_aValues[INPUT_ENTER].m_nCurrentValue, std::memory_order_relaxed);
			padEnterLast.store(a_pad->m_aValues[INPUT_ENTER].m_nLastValue, std::memory_order_relaxed);
			a_pad->m_aValues[INPUT_ENTER].m_nCurrentValue = 255;
		}
		if (pressExit.load(std::memory_order_relaxed)) {
			a_pad->m_aValues[INPUT_VEH_EXIT].m_nCurrentValue = 255;
			a_pad->m_aValues[INPUT_ENTER].m_nCurrentValue = 255;
		}
		if (pressThrottle.load(std::memory_order_relaxed)) {
			a_pad->m_aValues[INPUT_VEH_ACCELERATE].m_nCurrentValue = 255;  // DebugDriveThrottle: the player's W
		}
	}

	void OnIngameStartup()
	{
		logic.Reset();
		vehicleForMinecraft = false;
		jackSeen = false;  // (dragProof stays: cleared on the next ped if it was on)
		pressEnter = false;
		pressExit = false;
		hiddenPed = 0;  // the ped is going away with the old session
		knock = Knock{};
		knockdowns = 0;
		rotorProof = 0.0f;
		afterKnock = 0.0f;
		hitTracks.clear();
		debugRunCar = 0;
		vht.car = 0;
		vht.ped = 0;
		nextWayVehicle = 0;
		trace.active = false;
		NikoBody::OnIngameStartup();
		seatLoggedFor = 0;
		lastReason = nullptr;
	}

	Result Tick(const Frame& a_f)
	{
		++hitFrame;  // (vehicle hits: last frame's poses only count from the frame right before)
		if (!configured) {
			configured = true;
			logic = drive::Logic(Cfg().toggleStartsInMinecraft);
			LC_LOG("vehicles: %s (dik 0x%02X) enters a vehicle while Minecraft drives; %s (dik 0x%02X) toggles Minecraft/Niko mode; starting in %s mode",
				Cfg().vehicleKey.c_str(), Cfg().VehicleKeyDik(), Cfg().toggleKey.c_str(), Cfg().ToggleKeyDik(),
				logic.mode() == drive::Mode::kMinecraft ? "Minecraft" : "Niko");
		}
		const bool inGame = a_f.exists && !a_f.loading;
		const bool active = inGame && !a_f.paused;

		drive::Input in;
		in.dt = a_f.dt;
		in.inGame = inGame;
		in.paused = a_f.paused;
		in.dead = a_f.dead;
		in.inCar = a_f.inCar;
		in.gettingIn = a_f.exists && !a_f.inCar && S::IS_CHAR_GETTING_IN_TO_A_CAR(a_f.ped);
		in.cutscene = a_f.cutscene;
		in.scripted = a_f.scripted;
		in.puppeting = a_f.puppeting;
		const std::uint32_t pads = padFrames.load(std::memory_order_relaxed);
		padLiveRun = pads != padFramesSeen ? padLiveRun + 1 : 0;
		padFramesSeen = pads;
		releasedT = logic.entering() && !a_f.puppeting ? releasedT + (a_f.paused ? 0.0f : a_f.dt) : 0.0f;
		in.controlReady = a_f.exists && !a_f.puppeting && padLiveRun >= kPadLiveFrames && S::IS_PLAYER_CONTROL_ON(a_f.player) &&
		                  (PedStanding() || releasedT >= kStandWait);
		if (logic.entering() && a_f.exists && !a_f.puppeting && !a_f.inCar) {
			float vx = 0, vy = 0, vz = 0;
			S::GET_CHAR_VELOCITY(a_f.ped, &vx, &vy, &vz);
			in.moving = vx * vx + vy * vy > kMovingSpeed * kMovingSpeed;
		}
		in.toggles = togglePresses.exchange(0, std::memory_order_relaxed);
		in.vehicleActions = vehiclePresses.exchange(0, std::memory_order_relaxed);
		// Standing again? (after a vehicle or a fall, GTA keeps Niko until he is: DriveLogic)
		if (a_f.exists && !a_f.inCar && !a_f.dead) {
			in.ragdoll = S::IS_PED_RAGDOLL(a_f.ped) || S::IS_CHAR_GETTING_UP(a_f.ped);
			if (!a_f.puppeting) {
				float speed = 0.0f;
				S::GET_CHAR_SPEED(a_f.ped, &speed);
				const bool air = S::IS_CHAR_IN_AIR(a_f.ped), standing = PedStanding();
				in.upright = !in.ragdoll && !air && speed < kUprightSpeed && standing;
				std::snprintf(uprightWhy, sizeof(uprightWhy), "ragdoll/getting up %d, in the air %d, %.2f m/s, standing flag %d", in.ragdoll ? 1 : 0, air ? 1 : 0,
					speed, standing ? 1 : 0);
			}
		}
		// Knocked over: a car running into the puppeted player (here), a blast (Combat, last frame).
		if (Cfg().ragdollOnVehicleHit && active && a_f.puppeting && !a_f.inCar && logic.mode() == drive::Mode::kMinecraft && !knock.pending &&
			!logic.recovering()) {
			DetectVehicleHits(a_f);
		}
		in.knockdowns = knockdowns;
		knockdowns = 0;

		// ---- test hooks (DebugAutoToggle / DebugAutoVehicle) -----------------------------------------
		if (Cfg().debugAutoToggle && active && (debugToggleT += a_f.dt) >= kDebugToggleSeconds) {
			debugToggleT = 0.0f;
			++in.toggles;
			LC_LOG("DebugAutoToggle: toggling");
		}
		bool exitNow = false;
		if (Cfg().debugAutoVehicle && active) {
			debugCooldown -= a_f.dt;
			debugInCarT = a_f.inCar ? debugInCarT + a_f.dt : 0.0f;
			if (debugCooldown <= 0.0f && a_f.puppeting && !a_f.inCar && !logic.entering()) {
				// Traffic drives off and has drivers: park an empty car of our own next to Niko first
				// (once he stands on something: right after a load he may still be falling into place).
				float aboveGround = 99.0f;
				S::GET_CHAR_HEIGHT_ABOVE_GROUND(a_f.ped, &aboveGround);
				int interior = 0;
				S::GET_INTERIOR_FROM_CHAR(a_f.ped, &interior);
				if (const Car car = ClosestCar(a_f.ped, kDebugCarRadius); car.handle) {
					LC_LOG("DebugAutoVehicle: vehicle %d is %.1f m away; pressing the vehicle key", car.handle, car.distance);
					++in.vehicleActions;
					debugCooldown = kDebugCooldown;
				} else if (aboveGround > 2.0f || interior != 0) {
					// Indoors or up somewhere (a save can start in Roman's flat): out to the street.
					debugCooldown = 1.0f;
					if ((debugIndoorT += 1.0f) >= 6.0f && StartRelocation(a_f.ped, "DebugAutoVehicle", interior, aboveGround)) {
						debugIndoorT = 0.0f;
					}
				} else {
					debugIndoorT = 0.0f;
					if (debugCar && !debugCarRequested && S::DOES_VEHICLE_EXIST(debugCar)) {
						S::MARK_CAR_AS_NO_LONGER_NEEDED(&debugCar);  // left behind somewhere
					}
					const unsigned int model = S::GET_HASH_KEY("admiral");
					if (!debugCarRequested) {
						debugCarRequested = true;
						CStreaming::ScriptRequestModel(static_cast<std::int32_t>(model));
						LC_LOG("DebugAutoVehicle: requesting a test car model");
					}
					if (S::HAS_MODEL_LOADED(model)) {
						float x = 0, y = 0, z = 0;
						S::GET_OFFSET_FROM_CHAR_IN_WORLD_COORDS(a_f.ped, 2.5f, 2.5f, 0.0f, &x, &y, &z);
						S::CREATE_CAR(model, x, y, z, &debugCar, true);
						S::MARK_MODEL_AS_NO_LONGER_NEEDED(model);
						debugCarRequested = false;
						int driver = 0;
						if (Cfg().debugVehicleDriver && debugCar) {
							S::CREATE_RANDOM_CHAR_AS_DRIVER(debugCar, &driver);  // GTA's press then carjacks
							if (driver) {
								S::TASK_PAUSE(driver, 120000);  // ...a parked car: the driver waits instead of driving off
							}
						}
						LC_LOG("DebugAutoVehicle: test car %d parked at %.1f %.1f %.1f%s", debugCar, x, y, z, driver ? " with a driver" : "");
						debugCooldown = 6.0f;  // (GTA may need a moment before a fresh car can be entered)
					} else {
						debugCooldown = 0.25f;
					}
				}
			}
			if (debugCooldown <= 0.0f && a_f.inCar && debugInCarT >= kDebugExitAfter) {
				LC_LOG("DebugAutoVehicle: %.0f s in a vehicle; pressing GTA's exit control", debugInCarT);
				exitNow = true;
				debugExitCheckT = 3.0f;
				debugCooldown = kDebugCooldown;
			}
			// A parked test car can have its doors against a wall: out through the roof, then.
			if (debugExitCheckT > 0.0f && (debugExitCheckT -= a_f.dt) <= 0.0f && a_f.inCar) {
				int veh = 0;
				S::GET_CAR_CHAR_IS_USING(a_f.ped, &veh);
				float cx = 0, cy = 0, cz = 0, h = 0;
				if (veh && S::DOES_VEHICLE_EXIST(veh)) {
					S::GET_CAR_COORDINATES(veh, &cx, &cy, &cz);
					S::GET_CAR_HEADING(veh, &h);
					const float x = cx - std::cos(h * kDegToRad) * 2.5f, y = cy - std::sin(h * kDegToRad) * 2.5f;
					S::WARP_CHAR_FROM_CAR_TO_COORD(a_f.ped, x, y, cz + 0.5f);
					LC_LOG("DebugAutoVehicle: still in the vehicle 3 s after the exit press; WARP_CHAR_FROM_CAR_TO_COORD %.1f %.1f %.1f", x, y, cz + 0.5f);
				}
			}
		}
		WalkTestTick(a_f);
		if (active && DebugBailOutTick(a_f)) {
			exitNow = true;
			debugExitCheckT = 0.0f;
		}
		if (active) {
			DebugRunOverTick(a_f);
			DebugVehicleHitTick(a_f);
		}
		if (inGame) {
			DebugCutsceneTick(a_f);
		}
		// DebugDriveThrottle: in a car, GTA's accelerator held from 1 s to 1 + N s (the player driving
		// straight on), the car's speed logged.
		throttleT = a_f.inCar && active ? throttleT + a_f.dt : 0.0f;
		const bool throttle = Cfg().debugDriveThrottle > 0.0f && throttleT > 1.0f && throttleT < 1.0f + Cfg().debugDriveThrottle;
		pressThrottle.store(throttle, std::memory_order_relaxed);
		if (throttle && (throttleLogT -= a_f.dt) <= 0.0f) {
			throttleLogT = 0.5f;
			int veh = 0;
			float speed = 0.0f;
			S::GET_CAR_CHAR_IS_USING(a_f.ped, &veh);
			if (veh && S::DOES_VEHICLE_EXIST(veh)) {
				S::GET_CAR_SPEED(veh, &speed);
			}
			LC_LOG("DebugDriveThrottle: %.1f s on the accelerator, the car at %.1f m/s", throttleT - 1.0f, speed);
		}
		// Another module's test hook asked for the street (DebugRequestRoad).
		if (roadRequest && a_f.puppeting && active) {
			float aboveGround = 99.0f;
			S::GET_CHAR_HEIGHT_ABOVE_GROUND(a_f.ped, &aboveGround);
			int interior = 0;
			S::GET_INTERIOR_FROM_CHAR(a_f.ped, &interior);
			if (StartRelocation(a_f.ped, roadRequest, interior, aboveGround)) {
				roadRequest = nullptr;
			}
		}
		// DebugGiveWeapon: a GTA weapon for Niko, once (puppet mode holsters it, Game.cpp).
		static float giveT = 0.0f;
		if (Cfg().debugGiveWeapon > 0 && giveT >= 0.0f && active && (giveT += a_f.dt) >= 10.0f) {
			giveT = -1.0f;
			S::GIVE_WEAPON_TO_CHAR(a_f.ped, static_cast<unsigned>(Cfg().debugGiveWeapon), 60, false);
			unsigned now = 0;
			S::GET_CURRENT_CHAR_WEAPON(a_f.ped, &now);
			LC_LOG("DebugGiveWeapon: weapon %d given to Niko (current weapon now %u)", Cfg().debugGiveWeapon, now);
		}
		if (exitNow) {
			exitPressT = kExitPressSeconds;
		}
		exitPressT = std::max(0.0f, exitPressT - a_f.dt);
		pressExit.store(exitPressT > 0.0f, std::memory_order_relaxed);

		// ---- the verdict --------------------------------------------------------------------------------
		const bool wasEntering = logic.entering();
		const auto out = logic.Step(in);
		if (out.modeChanged) {
			LC_LOG("toggle: %s mode", logic.mode() == drive::Mode::kNiko ? "Niko" : "Minecraft");
			if (inGame) {
				ShowMode(logic.mode());
			}
		}
		if (logic.entering() && !wasEntering) {
			LC_LOG("vehicle key: handing Niko to GTA IV, then tapping its enter-vehicle control");
			enterClock = 0.0f;
			tapsThisAttempt = 0;
			gtaTookPress = false;
		}
		if (out.tapStarted) {
			tapsThisAttempt = out.tapStarted;
			LC_LOG("vehicle key: tap %d of GTA's enter control (%.2f s after the key; Niko %s)", out.tapStarted, enterClock,
				PedStanding() ? "stands" : "doesn't stand yet, waited long enough");
		}
		if (logic.entering() && a_f.exists) {
			// when GTA counts the released ped as standing again (log)
			const bool standing = PedStanding();
			if (standing != lastStanding) {
				LC_LOG("vehicle key: GTA counts Niko as %s (%.2f s after the key)", standing ? "standing" : "NOT standing", enterClock);
				lastStanding = standing;
			}
		} else {
			lastStanding = true;
		}
		if (out.accepted) {
			gtaTookPress = true;
			LC_LOG("GTA took the press (tap %d): Niko %s (%.2f s after the key)", tapsThisAttempt, in.gettingIn ? "is getting in" : "walks to a door", enterClock);
		}
		if (wasEntering && !logic.entering() && a_f.inCar) {
			LC_LOG("Niko is in a vehicle (%.2f s after the key)", enterClock);
		}
		enterClock += a_f.dt;
		if (logic.entering() && in.gettingIn && !sawGettingIn) {
			LC_LOG("Niko is getting into a vehicle (%.2f s after the key)", enterClock);
		}
		sawGettingIn = logic.entering() && (sawGettingIn || in.gettingIn);
		if (Cfg().debugInjectEnterKey) {
			// Test hook: the taps as real key events (SendInput, through Wine's DirectInput) instead
			// of pad writes, to compare with what GTA does for a real F.
			static bool injected = false;
			if (out.pressEnter != injected) {
				INPUT key{};
				key.type = INPUT_KEYBOARD;
				key.ki.wScan = Cfg().VehicleKeyDik();
				key.ki.dwFlags = KEYEVENTF_SCANCODE | (out.pressEnter ? 0 : KEYEVENTF_KEYUP);
				::SendInput(1, &key, sizeof(key));
				injected = out.pressEnter;
				if (out.pressEnter) {
					LC_LOG("DebugInjectEnterKey: real key down (scan code 0x%02X)", Cfg().VehicleKeyDik());
				}
			}
			pressEnter.store(false, std::memory_order_relaxed);
		} else {
			pressEnter.store(out.pressEnter, std::memory_order_relaxed);
		}
		if (out.fallbackEnter && a_f.exists) {
			EnterByOtherMeans(a_f.ped);
		}
		if (out.enterFailed) {
			LC_LOG("no vehicle entered after %.0f s; Minecraft takes the player back", drive::Logic::kGiveUpAfter);
		}
		recoverClock = logic.recovering() ? recoverClock + a_f.dt : 0.0f;
		if (out.recovered && logic.mode() == drive::Mode::kMinecraft) {
			LC_LOG("%s: Minecraft takes over%s%s", out.recoverCapped ? "Niko didn't get back up in time" : "Niko stands again", out.recoverCapped ? " (last: " : "",
				out.recoverCapped ? (std::string(uprightWhy) + ")").c_str() : "");
		}
		// A knockdown's ragdoll, once puppet mode has let go of Niko (frozen, he wouldn't fall).
		if (knock.pending) {
			knock.wait += a_f.dt;
			if (a_f.exists && !a_f.puppeting && !a_f.inCar && !a_f.dead) {
				if (knock.rotors) {
					// GTA IV's own spinning rotor chops a ped it touches (measured: 660 health in one go, which
					// Combat would hand to Minecraft as 66 damage on top of our strike's): his fall out of its
					// reach is invincible (collision-proof doesn't cover it).
					S::SET_CHAR_INVINCIBLE(a_f.ped, true);
					rotorProof = 1.0f;
				}
				afterKnock = 1.5f;
				if (knock.way == KnockWay::kGtaHit) {
					LC_LOG("knocked over (%s): Niko left standing in the vehicle's way (GTA's own hit)", knock.what);
				} else {
					S::SWITCH_PED_TO_RAGDOLL(a_f.ped, knock.ms, knock.ms, false, false, false, false);
					// World-direction force (APPLY_FORCE_TO_PED's 10th argument 0; Combat.cpp measured it).
					S::APPLY_FORCE_TO_PED(a_f.ped, 3, knock.gx * knock.force, knock.gy * knock.force, knock.force * knock.up, 0.0f, 0.0f, 0.0f, 0, 0, 1, 1);
					LC_LOG("knocked over (%s): Niko ragdolled for %d ms, pushed %.2f %.2f x %.1f (up %.1f)", knock.what, knock.ms, knock.gx, knock.gy, knock.force,
						knock.force * knock.up);
				}
				knock.pending = false;
			} else if (knock.wait > 1.0f) {
				LC_LOG("knockdown dropped (%s): Niko wasn't free to fall within 1 s", knock.what);
				knock.pending = false;
			}
		}
		TraceTick(a_f, out.recovered);
		if (rotorProof > 0.0f && (rotorProof -= a_f.dt) <= 0.0f) {
			if (a_f.exists && !a_f.puppeting) {
				S::SET_CHAR_INVINCIBLE(a_f.ped, false);  // (puppet mode sets its own)
			}
			LC_LOG("knockdown: Niko can be hurt by GTA again (out of the rotor's reach)");
		}
		if (afterKnock > 0.0f) {
			afterKnock -= a_f.dt;
			if (!a_f.exists || a_f.dead) {
				LC_LOG("knockdown: Niko died %.2f s after the knockdown's release (GTA killed him)", 1.5f - afterKnock);
				afterKnock = 0.0f;
			}
		}
		if (out.blocker != lastReason) {
			LC_LOG("GTA IV drives the player: %s", out.blocker ? out.blocker : "no, Minecraft may");
			lastReason = out.blocker;
		}

		Result r;
		r.blocker = out.blocker;
		r.hostDrives = out.hostDrives;
		r.inVehicle = out.inVehicle;
		r.resync = out.resync;
		r.heading = a_f.heading;
		if (debugRelocate > 0 && !out.hostDrives && a_f.exists) {
			// DebugAutoVehicle's move to the road: off puppet for a frame, move, then the usual resync.
			if (!a_f.puppeting && !debugRelocated) {
				S::SET_CHAR_COORDINATES(a_f.ped, debugRelocateTo[0], debugRelocateTo[1], debugRelocateTo[2]);
				debugRelocated = true;
			}
			if (--debugRelocate > 0) {
				r.blocker = "test hook: moving Niko to the road";
				r.hostDrives = true;
			} else {
				r.resync = true;
				debugRelocated = false;
			}
		}
		if (out.inVehicle) {
			int veh = 0;
			S::GET_CAR_CHAR_IS_USING(a_f.ped, &veh);
			// GET_CHAR_COORDINATES gives the vehicle's origin for a ped inside one; the ped's own matrix
			// sits in its seat (the driver's is left of and in front of the middle).
			float px = 0, py = 0, pz = 0;
			S::GET_CHAR_COORDINATES(a_f.ped, &px, &py, &pz);
			float sx = px, sy = py, sz = pz;
			CPed* pedObj = FindPlayerPed();
			if (pedObj && pedObj->m_pMatrix) {
				const auto& m = pedObj->m_pMatrix->pos;
				if (std::fabs(m.x - px) < 3.0f && std::fabs(m.y - py) < 3.0f && std::fabs(m.z - pz) < 3.0f) {
					sx = m.x, sy = m.y, sz = m.z;
				}
			}
			if (veh && S::DOES_VEHICLE_EXIST(veh)) {
				S::GET_CAR_HEADING(veh, &r.heading);
				if (seatLoggedFor != veh) {
					seatLoggedFor = veh;
					float ground = 0;
					S::GET_GROUND_Z_FOR_3D_COORD(px, py, pz, &ground);
					LC_LOG("in vehicle %d (heading %.1f): origin %.2f %.2f %.2f, seat (ped matrix) %.2f %.2f %.2f = %.2f m off the origin, %.2f m above "
						   "the ground %.2f; the rider's feet go %.2f m below the seat (VehicleSeatDrop)",
						veh, r.heading, px, py, pz, sx, sy, sz, std::sqrt((sx - px) * (sx - px) + (sy - py) * (sy - py) + (sz - pz) * (sz - pz)), sz - ground,
						ground, Cfg().vehicleSeatDrop);
				}
			}
			r.seatFeet = { sx, sy, sz - Cfg().vehicleSeatDrop };
		} else {
			seatLoggedFor = 0;
		}

		// While GTA animates Niko the Minecraft body may follow his skeleton instead (NikoBody.h);
		// else, in a vehicle, Minecraft's player sits on its mount in the seat: Niko would be in the way.
		// Handing back to Minecraft (the teleport handshake, then puppet mode, a frame or a few later):
		// the body stays on Niko and he stays hidden until puppet mode has him (it hides him itself), so
		// no frame shows him in between.
		static drive::Why lastWhy = drive::Why::kNone;
		static float      handBackT = 0.0f;
		if (out.hostDrives) {
			lastWhy = out.why;
			handBackT = 0.0f;
		} else if (out.resync && logic.mode() == drive::Mode::kMinecraft && a_f.mcInWorld) {
			handBackT = kHandBackSeconds;
		}
		handBackT = a_f.puppeting ? 0.0f : std::max(0.0f, handBackT - a_f.dt);
		const bool handingBack = handBackT > 0.0f && !out.hostDrives && lastWhy != drive::Why::kNone;
		// The game moved the player (a warp through a door, a script): Game's teleport handshake runs and
		// Minecraft holds its player until his ground has arrived; Niko stays hidden (Game leaves him so)
		// under the Minecraft body standing on his skeleton meanwhile.
		const bool resyncBody = a_f.resyncing && !a_f.puppeting && !out.hostDrives && !handingBack && logic.mode() == drive::Mode::kMinecraft;
		// The screen fading out (a cutscene's or a mission scene's end, a script's warp) counts as not in game,
		// so GTA "drove" no one for its frames: the body went off and Niko showed while it faded. The body stays
		// as it was until the game is back (then the teleport handshake has the player, resyncBody).
		static bool drovePrev = false;
		const bool  fadeHold = a_f.loading && a_f.exists && !a_f.dead && !a_f.puppeting && drovePrev && lastWhy != drive::Why::kNone &&
		                      logic.mode() == drive::Mode::kMinecraft && !out.hostDrives && !handingBack && !resyncBody;
		drovePrev = out.hostDrives || handingBack || resyncBody || fadeHold;
		const drive::Why bodyWhy = handingBack || fadeHold ? lastWhy : resyncBody ? drive::Why::kRagdoll : out.why;
		const int  bodyPed = NikoBody::Target(a_f.exists && !a_f.dead ? a_f.ped : 0, bodyWhy, out.hostDrives || handingBack || resyncBody || fadeHold, a_f.mcInWorld);
		static int loggedResync = 0;  // 0 none, 1 with the body, 2 without (no body from Minecraft yet)
		const int  resyncState = resyncBody ? (bodyPed ? 1 : 2) : 0;
		if (resyncState != loggedResync) {
			loggedResync = resyncState;
			LC_LOG("%s", resyncState == 1   ? "teleport handshake: Niko hidden under the Minecraft body until Minecraft arrives"
			             : resyncState == 2 ? "teleport handshake: no Minecraft body to show yet (Niko as puppet mode left him: hidden)"
			                                : "teleport handshake over");
		}
		const bool hide = out.inVehicle && Cfg().hideNikoInVehicle && logic.mode() == drive::Mode::kMinecraft && a_f.mcInWorld;
		// GTA's peds drag a seated player out of a stopped vehicle (a ped fighting him, one taking the car):
		// not while the Minecraft player is in creative or spectator (kMcCreative, invulnerable), where
		// SET_CHAR_CANT_BE_DRAGGED_OUT holds while he sits in one in Minecraft mode. Survival and adventure
		// keep GTA's own dragging; Niko mode (plain GTA IV) and getting out clear it.
		// (No ped, e.g. dead: it is cleared once he is back, the same ped. Without this frame's McState the
		// last game mode holds.)
		if (a_f.haveMc) {
			mcCreative = a_f.mcCreative;
		}
		const bool noDrag = a_f.exists && !a_f.dead && a_f.inCar && logic.mode() == drive::Mode::kMinecraft && mcCreative;
		if (noDrag != dragProof && a_f.exists) {
			S::SET_CHAR_CANT_BE_DRAGGED_OUT(a_f.ped, noDrag);
			dragProof = noDrag;
			LC_LOG("%s", noDrag ? "seated in Minecraft mode, creative: GTA's peds can't drag the player out (SET_CHAR_CANT_BE_DRAGGED_OUT)"
			                    : "the player can be dragged out of vehicles again");
		}
		// Who tries anyway (log): the ped jacking him, and what kind of ped it is.
		const bool jacked = a_f.exists && a_f.inCar && S::IS_PED_BEING_JACKED(a_f.ped);
		if (jacked && !jackSeen) {
			const int jacker = FindJacker(a_f.ped);
			unsigned  type = 0;
			if (jacker) {
				S::GET_PED_TYPE(jacker, &type);
			}
			LC_LOG("a ped tries to drag the player out of the vehicle (%s): ped %d, ped type %u%s",
				noDrag ? "Minecraft mode, creative: refused" : logic.mode() == drive::Mode::kNiko ? "Niko mode: GTA's way" : "Minecraft mode, survival: GTA's way", jacker,
				type, type == 2 ? " (police)" : type == 0 || type == 1 ? " (civilian)" : "");
		}
		jackSeen = jacked;
		if (bodyPed) {
			SetHidden(bodyPed, true, "the Minecraft body follows his animation");
		} else if (a_f.exists) {
			SetHidden(a_f.ped, hide, "Minecraft's player rides in the vehicle");
		}

		vehicleForMinecraft = out.hostDrives && out.why == drive::Why::kVehicle;  // (VehicleInMinecraftMode)
		auto& st = Game::State();
		st.hostDrives = out.hostDrives;
		st.inVehicle = out.inVehicle;
		st.nikoMode = logic.mode() == drive::Mode::kNiko;
		// (A mission script's scene hides Minecraft's HUD as a cutscene does.)
		st.cutscene = out.hostDrives && (out.why == drive::Why::kCutscene || out.why == drive::Why::kScript);
		st.padLocked = out.padLocked && !a_f.paused;
		static bool lastPadLocked = false;
		if (out.padLocked != lastPadLocked) {
			lastPadLocked = out.padLocked;
			LC_LOG("%s", out.padLocked ? "Niko gets back up: the player's pad is zeroed until Minecraft takes over" : "the player's pad is GTA's or Minecraft's again");
		}
		return r;
	}

	void AfterPuppetDecision()
	{
		if (hiddenPed) {
			NikoBody::Hide(hiddenPed, true);
		}
	}

	void DebugRequestRoad(const char* a_who)
	{
		roadRequest = a_who;
	}

	bool KnockedOver()
	{
		return logic.recovering() && logic.recoveringFrom() == drive::Why::kRagdoll && logic.mode() == drive::Mode::kMinecraft;
	}

	bool VehicleInMinecraftMode()
	{
		return vehicleForMinecraft && logic.mode() == drive::Mode::kMinecraft;
	}

	void KnockDown(float a_gx, float a_gy, float a_force, int a_ragdollMs, const char* a_what)
	{
		if (!Cfg().ragdollOnVehicleHit || knock.pending || logic.recovering() || logic.mode() != drive::Mode::kMinecraft) {
			return;
		}
		const float len = std::hypot(a_gx, a_gy);
		StartKnock(len > 1e-3f ? a_gx / len : 0.0f, len > 1e-3f ? a_gy / len : 0.0f, a_force, a_ragdollMs, a_what);
		LC_LOG("%s: knocked over (force %.1f, capped %.1f; ragdoll %d ms)", a_what, a_force, knock.force, a_ragdollMs);
		if (CPed* p = FindPlayerPed(); p && p->m_pMatrix) {
			const float at[3] = { p->m_pMatrix->pos.x, p->m_pMatrix->pos.y, p->m_pMatrix->pos.z };
			StartTrace(at, a_what);
		}
	}

	int DebugPuppetTarget(GtaVec& a_feet)
	{
		auto& w = walk;
		if (!w.active) {
			return 0;
		}
		if (w.needStart) {
			w.needStart = false;
			w.start = a_feet;
			w.target = a_feet;
		}
		// Moves the target toward a point at a_speed m/s; true once there.
		const auto toward = [&](double a_x, double a_y, float a_speed) {
			const double dx = a_x - w.target.x, dy = a_y - w.target.y, d = std::sqrt(dx * dx + dy * dy), step = a_speed * w.dt;
			if (d <= step || d < 1e-6) {
				w.target.x = a_x, w.target.y = a_y;
				return true;
			}
			w.target.x += dx / d * step, w.target.y += dy / d * step;
			return false;
		};
		if (w.step == 3) {
			// 10 m out along the path (through the car at 5 m) and back, 6 s.
			const float u = std::min(w.t / 6.0f, 1.0f), d = 10.0f * (u < 0.5f ? u * 2.0f : 2.0f - u * 2.0f);
			w.target.x = w.start.x + w.dir[0] * d;
			w.target.y = w.start.y + w.dir[1] * d;
		} else if (w.step == 4 && w.ped && !w.pedGone && S::DOES_CHAR_EXIST(w.ped)) {
			float x = 0, y = 0, z = 0;
			S::GET_CHAR_COORDINATES(w.ped, &x, &y, &z);
			toward(x, y, 8.0f);  // into the pedestrian, and stay there
		} else if (w.step == 5) {
			toward(a_feet.x, a_feet.y, 6.0f);  // back to where Minecraft's player is
		}
		w.target.z = a_feet.z;
		a_feet = w.target;
		return w.round == 0 ? 1 : 2;
	}
}

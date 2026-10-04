// Unity-built into dllmain.cpp (needs IV-SDK). See Game.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "game"
#include "Game.h"

#include "BlockyCity.h"
#include "Collision.h"
#include "Combat.h"
#include "Config.h"
#include "Coords.h"
#include "Doors.h"
#include "Hazards.h"
#include "HostDrive.h"
#include "Input.h"
#include "Link.h"
#include "Log.h"
#include "Perf.h"
#include "ViewportRoom.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>

namespace lc::Game
{
	namespace
	{
		namespace S = ::Scripting;

		constexpr double kGameTeleportMetres = 5.0;  // the game moved the player farther than this: resync Minecraft
		constexpr float  kSettleSeconds = 1.0f;      // after a teleport / world change, before harvesting collision
		constexpr float  kInteriorDebounce = 0.5f;   // seconds an interior id must hold before it counts
		constexpr float  kReassertSeconds = 0.5f;    // re-apply HUD/control state this often while puppeting
		constexpr float  kTopProbeZ = 1000.0f;

		Shared shared;

		// ---- everything below is only touched on the game thread ------------------------------------
		proto::McState mc{};              // last McState read cleanly (never a torn copy)
		std::uint64_t  mcGoodMs = 0;      // GetTickCount64() of that read; 0 = none yet
		bool           mcWasAlive = false;
		std::uint32_t  lastMcPid = 0;
		std::uint32_t  linkGeneration = 0;
		// Starts somewhere new each run, so a Minecraft still acknowledging the last run's teleport
		// can't be taken for having arrived at this run's.
		std::uint32_t teleportSeq = [] {
			LARGE_INTEGER t;
			::QueryPerformanceCounter(&t);
			return static_cast<std::uint32_t>(t.QuadPart) | 1u;
		}();
		bool          teleportPending = true;
		std::uint32_t lastAckLogged = 0;
		float         holdMismatch = 0.0f;
		std::uint32_t worldId = 0;
		std::uint32_t epoch = 0;
		std::uint32_t interiorCandidate = 0;
		float         interiorTimer = 0.0f;
		GtaVec        lastSetFeet{};
		bool          haveLastSet = false;
		float         settleTimer = kSettleSeconds;
		float         yaw = 0.0f, pitch = 0.0f;  // authoritative look, MC degrees
		bool          lookInit = false;
		bool          wasMenuOpen = true;
		bool          wasScreenOpen = false;

		// root -> feet offset of the player ped (GET_CHAR_COORDINATES is the ped's root, ~1 m up)
		float  rootToFeet = 1.0f;
		double measureSum = 0.0, measureHagSum = 0.0;
		int    measureCount = 0;
		bool   measured = false;

		// puppet mode
		bool        puppeting = false;
		int         puppetPed = 0;
		int         puppetPlayer = 0;
		float       reassertTimer = 0.0f;
		int         pedHidden = -1;  // -1 unknown, 0 visible, 1 hidden
		unsigned    holsteredWeapon = 0;  // GTA's weapon put away for puppet mode (0: none)
		int         scriptCam = 0;
		const char* lastBlocker = nullptr;

		// camera pose for this frame (GTA space), consumed by Camera() / the scripted camera
		struct CamPose
		{
			bool  valid = false;
			float pos[3]{};
			float yaw = 0.0f, pitch = 0.0f;  // MC degrees (pitch includes the walk bob)
			float fovDeg = 70.0f;            // Minecraft's vertical FOV
		} camPose;
		float         zoom = 0.0f;
		std::uint32_t zoomMode = 0;
		bool          onFootForCamera = false;  // Tick's verdict, for the camera-row discovery
		float         gtaCameraRoomCheck = 0.0f;  // seconds GTA's own camera's room is still checked (ViewportRoom)

		// Which CMatrix row (0 right, 1 up, 2 at) holds the camera's right / forward / up, and its sign.
		struct Rows
		{
			int   idx[3] = { 0, 1, 2 };
			float sign[3] = { 1.0f, 1.0f, 1.0f };
			bool  known = false;
			bool  operator==(const Rows& o) const
			{
				return idx[0] == o.idx[0] && idx[1] == o.idx[1] && idx[2] == o.idx[2] && sign[0] == o.sign[0] && sign[1] == o.sign[1] && sign[2] == o.sign[2];
			}
		} rows;
		Rows rowsCandidate;
		int  rowsVotes = 0;
		bool rowsConfigured = false;
		bool rowsPinned = false;   // CameraRows=auto: the known-good rows, discovery only cross-checks them
		bool rowsChecked = false;

		// Minecraft's 20 Hz ticks, interpolated on our clock exactly like SkyCraft (see Interpolate).
		struct TickRec
		{
			proto::McState s;
			std::int64_t   at;     // when it happened (QPC), on the locked rhythm
			int            slots;  // ticks since the one we saw before (2+: we missed one)
		};
		std::deque<TickRec>    tickHistory;
		std::int64_t           lastFrameQpc = 0;
		int                    stampOutliers = 0;
		double                 renderDelayMs = 10.0;
		std::array<double, 40> tickDue{};
		std::size_t            tickDueNext = 0;
		bool                   tickDueInit = false;

		struct Motion
		{
			std::uint32_t frames = 0, ticks = 0, lateFrames = 0, stampSamples = 0;
			double        frameMsSum = 0.0, frameMsMax = 0.0, stampErrMs = 0.0;
		} motion;

		struct Stats
		{
			std::uint32_t skyWrites = 0, mcReads = 0, mcReadFails = 0, events = 0, cameraWrites = 0, frames = 0;
		} stats;
		std::uint64_t lastStatsMs = 0;
		std::uint32_t lastSkyWrites = 0;
		bool          firstSkyWriteLogged = false;

		std::int64_t Qpc()
		{
			LARGE_INTEGER t;
			::QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		double QpcPerMs()
		{
			static const double f = [] {
				LARGE_INTEGER q;
				::QueryPerformanceFrequency(&q);
				return double(q.QuadPart) / 1000.0;
			}();
			return f;
		}

		const Config& Cfg() { return Config::Get(); }

		// Minecraft's vertical FOV -> CCam::m_fFOV / SET_CAM_FOV (Config FovMode).
		float GtaFov(float a_mcVerticalDeg)
		{
			if (Cfg().fovMode == Config::FovMode::kHorizontal43) {
				return 2.0f * std::atan(std::tan(a_mcVerticalDeg * 0.5f * kDegToRad) * (4.0f / 3.0f)) * kRadToDeg;
			}
			return a_mcVerticalDeg;
		}

		void ResetCollision(const char* a_why)
		{
			++epoch;
			Collision::Get().Reset(epoch);
			settleTimer = kSettleSeconds;
			LC_LOG("collision epoch %u (%s)", epoch, a_why);
		}

		// ---- tick interpolation (port of SkyCraft Game.cpp) ------------------------------------------
		struct Pose
		{
			McVec feet, eye;
			float bobPhase, bobAmount;
		};

		// Interpolate Minecraft's 20 Hz physics ticks on our own clock (what MC's renderer does with
		// partial ticks): keep a short history and render slightly in the past, so the next tick has
		// always arrived: pure interpolation, never extrapolation. The render delay follows how late
		// ticks have been over the last 2 s.
		Pose Interpolate()
		{
			Pose p{ { mc.x, mc.y, mc.z }, { mc.eyeX, mc.eyeY, mc.eyeZ }, mc.bobPhase, mc.bobAmount };
			if (mc.tickQpc == 0 || mc.tickMs <= 0.0f) {
				return p;
			}
			const double       qpcPerMs = QpcPerMs();
			const std::int64_t period = std::max<std::int64_t>(1, std::llround(double(mc.tickMs) * qpcPerMs));
			const std::int64_t now = Qpc();
			if (tickHistory.empty() || tickHistory.back().s.tickQpc != mc.tickQpc) {
				if (!tickHistory.empty() && mc.tickQpc < tickHistory.back().s.tickQpc) {
					tickHistory.clear();  // Minecraft restarted
				}
				TickRec tick{ mc, mc.tickQpc, 1 };
				if (!tickHistory.empty()) {
					auto&              last = tickHistory.back();
					const std::int64_t n = std::llround(double(mc.tickQpc - last.at) / double(period));
					const std::int64_t err = mc.tickQpc - (last.at + n * period);
					if (n == 0 && last.slots >= 2) {
						// Minecraft ran two ticks in one frame and we saw both: the first one carries
						// the second's stamp. It belongs a tick earlier.
						last.at -= period;
						last.slots -= 1;
						tick.at = last.at + period;
					} else if (n >= 1 && n <= 10 && std::llabs(err) < period * 3 / 10) {
						tick.at = last.at + n * period + err / 16;  // the rhythm is exact; the stamps are noisy
						tick.slots = static_cast<int>(n);
						stampOutliers = 0;
						motion.stampErrMs += std::fabs(double(err)) / qpcPerMs;
						++motion.stampSamples;
					} else if (n <= 10 && ++stampOutliers < 3) {
						tick.slots = static_cast<int>(std::max<std::int64_t>(n, 1));
						tick.at = last.at + tick.slots * period;  // one odd stamp (a hitch): keep the rhythm
					} else {
						stampOutliers = 0;  // lost the rhythm (a pause, a new tick rate): start from this stamp
					}
				}
				// Was it already due on our last frame? Then rendering had to wait for it.
				if (lastFrameQpc != 0) {
					if (!tickDueInit) {
						tickDue.fill(renderDelayMs - 1.0);
						tickDueInit = true;
					}
					const double dueMs = double(lastFrameQpc - tick.at) / qpcPerMs;
					if (dueMs < 30.0) {  // later than that is a hitch, not a pattern to wait for
						tickDue[tickDueNext++ % tickDue.size()] = dueMs;
					}
				}
				tickHistory.push_back(tick);
				if (tickHistory.size() > 8) {
					tickHistory.pop_front();
				}
				++motion.ticks;
			}

			// The render delay grows 2% slower than real time and shrinks 0.2% faster.
			const double frameMs = lastFrameQpc != 0 ? double(now - lastFrameQpc) / qpcPerMs : 0.0;
			lastFrameQpc = now;
			if (tickDueInit) {
				const double target = std::clamp(*std::max_element(tickDue.begin(), tickDue.end()) + 1.0, 4.0, 30.0);
				const double dt = std::min(frameMs, 100.0) / 1000.0;
				renderDelayMs = target > renderDelayMs ? std::min(target, renderDelayMs + 20.0 * dt) : std::max(target, renderDelayMs - 2.0 * dt);
			}
			const std::int64_t renderQpc = now - std::llround(renderDelayMs * qpcPerMs);

			// The feet go through each tick's start (prev) and end (cur) positions.
			std::size_t i = 0;
			for (std::size_t k = tickHistory.size(); k-- > 0;) {
				if (tickHistory[k].at <= renderQpc) {
					i = k;
					break;
				}
			}
			const TickRec& tick = tickHistory[i];
			const TickRec* next = i + 1 < tickHistory.size() ? &tickHistory[i + 1] : nullptr;
			const double   ticks = double(renderQpc - tick.at) / double(period);
			const double   t = std::clamp(ticks, 0.0, 1.0);
			const auto&    s = tick.s;
			p.feet = { s.prevX + (s.curX - s.prevX) * t, s.prevY + (s.curY - s.prevY) * t, s.prevZ + (s.curZ - s.prevZ) * t };
			double eyeHeight = s.tickEyeO + (s.tickEye - s.tickEyeO) * t;
			p.bobPhase = -(s.walkDist + (s.walkDist - s.walkDistO) * static_cast<float>(t));
			p.bobAmount = s.bobO + (s.bob - s.bobO) * static_cast<float>(t);
			if (ticks > 1.0 && next) {
				// Past this tick's end, and the next tick we have starts later: Minecraft ran one we
				// never saw. Carry on from this tick's end to the next one's start.
				const auto&  n = next->s;
				const double gap = double(next->at - (tick.at + period));
				const double u = gap > 0.0 ? std::clamp(double(renderQpc - (tick.at + period)) / gap, 0.0, 1.0) : 1.0;
				p.feet = { s.curX + (n.prevX - s.curX) * u, s.curY + (n.prevY - s.curY) * u, s.curZ + (n.prevZ - s.curZ) * u };
				eyeHeight = s.tickEye + (n.tickEyeO - s.tickEye) * u;
				const float endPhase = -(s.walkDist + (s.walkDist - s.walkDistO));
				p.bobPhase = endPhase + (-n.walkDist - endPhase) * static_cast<float>(u);
				p.bobAmount = s.bob + (n.bobO - s.bob) * static_cast<float>(u);
			} else if (ticks > 1.0) {
				++motion.lateFrames;  // the next tick hasn't arrived: the player stands still this frame
			}
			p.eye = { p.feet.x, p.feet.y + eyeHeight, p.feet.z };
			++motion.frames;
			motion.frameMsSum += frameMs;
			motion.frameMsMax = std::max(motion.frameMsMax, frameMs);
			return p;
		}

		// Camera pose from the interpolated eye: Minecraft's walk bob (GameRenderer.bobView, as a
		// camera offset: sway sideways, lift, dip the view) and its F5 third-person camera.
		void BuildCamPose(const Pose& a_pose, float a_dt)
		{
			const float phase = a_pose.bobPhase * kPi;
			const float bob = a_pose.bobAmount;
			const float sideBlocks = -std::sin(phase) * bob * 0.5f;
			const float liftBlocks = std::fabs(std::cos(phase) * bob);
			const float bobPitchDeg = std::fabs(std::cos(phase - 0.2f) * bob) * 5.0f;

			const GtaBasis look = LookBasis(yaw, 0.0f);
			const GtaVec   eye = McToGta(a_pose.eye);
			float          pos[3] = { static_cast<float>(eye.x) + look.right[0] * sideBlocks, static_cast<float>(eye.y) + look.right[1] * sideBlocks,
                         static_cast<float>(eye.z) + liftBlocks };

			const bool  mirrored = mc.cameraMode == 2;
			const float camYaw = mirrored ? yaw + 180.0f : yaw;
			const float lookPitch = mirrored ? -pitch : pitch;
			// Minecraft's zoom pulls in the moment something is behind the player and eases back out.
			const bool detached = mc.cameraMode != 0 && mc.cameraDistance > 0.0f;
			if (!detached || zoomMode != mc.cameraMode || mc.cameraDistance < zoom) {
				zoom = detached ? mc.cameraDistance : 0.0f;
			} else {
				zoom += (mc.cameraDistance - zoom) * (1.0f - std::exp(-std::max(a_dt, 0.0f) / 0.2f));
			}
			zoomMode = mc.cameraMode;
			if (zoom > 0.0f) {
				const GtaBasis cam = LookBasis(camYaw, lookPitch);
				for (int k = 0; k < 3; ++k) {
					pos[k] -= cam.forward[k] * zoom;
				}
			}
			camPose.valid = true;
			std::copy(pos, pos + 3, camPose.pos);
			camPose.yaw = camYaw;
			camPose.pitch = lookPitch + bobPitchDeg;
			camPose.fovDeg = mc.fovDeg > 1.0f && mc.fovDeg < 170.0f ? mc.fovDeg : 70.0f;
		}

		// ---- puppet mode --------------------------------------------------------------------------------
		void DestroyScriptCam()
		{
			if (scriptCam) {
				S::SET_CAM_ACTIVE(scriptCam, false);
				S::ACTIVATE_SCRIPTED_CAMS(false, false);
				S::DESTROY_CAM(scriptCam);
				scriptCam = 0;
				LC_LOG("scripted camera destroyed");
			}
		}

		void ApplyPuppetState(int a_player, int a_ped)
		{
			// Player control stays on (PuppetPlayerControl): without it GTA's peds and cops lose interest
			// in the player (Combat.h). The pad is zeroed while puppeting (Input), so control on doesn't
			// let GTA move him. Collision stays off (PuppetCollision, a test hook).
			if (S::IS_PLAYER_CONTROL_ON(a_player) != Cfg().puppetPlayerControl) {
				S::SET_PLAYER_CONTROL(a_player, Cfg().puppetPlayerControl);
			}
			if (Cfg().freezePed) {
				S::FREEZE_CHAR_POSITION(a_ped, true);
			}
			S::SET_CHAR_COLLISION(a_ped, Cfg().puppetCollision);
			// Combat keeps the ped vulnerable (on a refilled health buffer) to forward GTA's damage.
			S::SET_CHAR_INVINCIBLE(a_ped, !Combat::OwnsPlayerHealth());
			// GtaHud: leave GTA's HUD and radar alone, so both HUDs show and GTA hides its own where
			// it normally does (cutscenes, menus, missions). Off: Minecraft's HUD only.
			if (!Cfg().gtaHud) {
				S::DISPLAY_HUD(false);
				S::DISPLAY_RADAR(false);
			}
			// GTA's weapon goes away (its HUD showed Niko's pistol next to Minecraft's sword), also one a
			// script hands him meanwhile; the last one comes back in Niko mode (not while GTA only
			// animates him for Minecraft mode: vehicles, getting back up).
			unsigned weapon = 0;
			if (S::GET_CURRENT_CHAR_WEAPON(a_ped, &weapon) && weapon != WEAPON_UNARMED) {
				holsteredWeapon = weapon;
				S::SET_CURRENT_CHAR_WEAPON(a_ped, WEAPON_UNARMED, true);
				LC_LOG("GTA weapon %u holstered while Minecraft drives", weapon);
			}
		}

		void EnterPuppet(int a_player, int a_ped, const GtaVec& a_feet)
		{
			puppeting = true;
			puppetPlayer = a_player;
			puppetPed = a_ped;
			pedHidden = -1;
			reassertTimer = kReassertSeconds;
			ApplyPuppetState(a_player, a_ped);
			shared.puppeting = true;
			Input::SetCapture(true);
			LC_LOG("puppet ON: Minecraft drives the player (ped %d at GTA %.2f %.2f %.2f, camera %s, root->feet %.2f m)", a_ped, a_feet.x, a_feet.y, a_feet.z,
				Cfg().cameraMode == Config::CameraMode::kScripted ? "scripted" : "final", rootToFeet);
		}

		void SettleOnGround(int a_ped);

		void LeavePuppet(const char* a_reason, bool a_restore = true, bool a_keepHidden = false)
		{
			if (!puppeting) {
				return;
			}
			puppeting = false;
			shared.puppeting = false;
			camPose.valid = false;
			if (a_restore) {
				if (puppetPed && S::DOES_CHAR_EXIST(puppetPed)) {
					SettleOnGround(puppetPed);
					S::FREEZE_CHAR_POSITION(puppetPed, false);
					S::SET_CHAR_COLLISION(puppetPed, true);
					S::SET_CHAR_INVINCIBLE(puppetPed, false);
					if (!a_keepHidden) {
						S::SET_CHAR_VISIBLE(puppetPed, true);
					}
				}
				S::SET_PLAYER_CONTROL(puppetPlayer, true);
				S::DISPLAY_HUD(true);
				S::DISPLAY_RADAR(true);
				DestroyScriptCam();
			} else {
				scriptCam = 0;  // the game is reloading: its cameras are gone anyway
				holsteredWeapon = 0;
			}
			Input::SetCapture(false);
			Input::ReleaseAll();
			puppetPed = 0;
			haveLastSet = false;
			LC_LOG("puppet OFF: %s", a_reason);
		}

		// ---- placing the puppeted ped ----------------------------------------------------------------------
		// Every SET_CHAR_COORDINATES* native ends in CTheScripts::ClearSpaceForMissionEntity (1.0.8.0:
		// 0x8B1390, called from the natives' common body at 0x8B2BF0): each ambient vehicle and ped
		// whose bounds touch the player's at the destination is deleted. Puppet mode places the ped
		// every frame, so every car and pedestrian the player walked into vanished. The common body
		// moves an on-foot ped with one virtual call (vtable +0x7C: const position*, float -10, bool
		// "keep tasks" = injured), then clears the space (SET_CHAR_COORDINATES_NO_OFFSET also warps
		// the player's group along). PuppetMove=direct makes just that call. The code bytes are checked
		// once; another game version, or a ped in a vehicle or injured, falls back to the native.
		int    directMove = -1;  // -1 not checked yet, 0 unavailable, 1 available
		double moveDriftMax = 0.0;  // stats: how far the ped was from where it was put last frame
		bool   haveMoveCheck = false;
		float  moveCheck[3]{};

		bool DirectMoveAvailable()
		{
			if (directMove < 0) {
				directMove = 0;
				if (plugin::gameVer == plugin::VERSION_1080) {
					// mov edx,[esi]; mov edx,[edx+7Ch]; push eax; push ecx (the move), then push esi; call 0x8B1390 (the clearing)
					static constexpr std::uint8_t kMove[] = { 0x8B, 0x16, 0x8B, 0x52, 0x7C, 0x50, 0x51 };
					const auto*                   base = reinterpret_cast<const std::uint8_t*>(AddressSetter::gBaseAddress);
					std::int32_t                  rel = 0;
					std::memcpy(&rel, base + 0x4B2D7E, sizeof rel);
					directMove = std::memcmp(base + 0x4B2D2F, kMove, sizeof kMove) == 0 && base[0x4B2D7C] == 0x56 && base[0x4B2D7D] == 0xE8 &&
					                     rel == 0x4B1390 - 0x4B2D82
					                 ? 1
					                 : 0;
				}
				if (Cfg().puppetMove == "native") {
					LC_LOG("puppet move: SET_CHAR_COORDINATES_NO_OFFSET (PuppetMove=native: deletes the cars and peds the player walks into)");
				} else if (directMove) {
					LC_LOG("puppet move: direct (the natives' own move, vtable +0x7C, without their clearing of the destination)");
				} else {
					LC_LOG("puppet move: WARNING: the natives' move isn't where 1.0.8.0 has it; using SET_CHAR_COORDINATES_NO_OFFSET (it deletes the cars and "
						   "peds the player walks into)");
				}
			}
			return directMove == 1;
		}

		// Puts the ped's root at a_x a_y a_z. a_native: SET_CHAR_COORDINATES_NO_OFFSET regardless.
		void MovePed(int a_ped, float a_x, float a_y, float a_z, bool a_native)
		{
			CPed* obj = !a_native && DirectMoveAvailable() ? CPools::ms_pPedPool->GetAt(static_cast<std::uint32_t>(a_ped)) : nullptr;
			if (!obj || obj->m_bInjured || obj->m_nPedFlags2.bInCar) {
				S::SET_CHAR_COORDINATES_NO_OFFSET(a_ped, a_x, a_y, a_z);
				return;
			}
			alignas(16) float pos[4] = { a_x, a_y, a_z, 0.0f };  // the native passes a 16-byte aligned vector
			using SetPosition = void(__thiscall*)(CPed*, float*, float, bool);
			void* const fn = (*reinterpret_cast<void***>(obj))[0x7C / 4];
			static void* loggedFn = nullptr;
			if (fn != loggedFn) {
				loggedFn = fn;
				LC_LOG("puppet move: ped %d (vtable %p) moves through %p", a_ped, *reinterpret_cast<void**>(obj), fn);
			}
			// Keep the player's tasks while GTA's phone is out (its task holds the phone; the move clears
			// them otherwise, like SET_CHAR_COORDINATES does).
			reinterpret_cast<SetPosition>(fn)(obj, pos, -10.0f, Input::PhoneOut());
		}

		// Puppet mode lets go: GTA's physics takes the ped back. Minecraft's feet are often a few cm
		// above where GTA's physics rests the ped (its collision is GTA's ground sampled into
		// triangles, kerbs and camber differ), so the ped dropped them and spent a second or more
		// "landing": GTA counts it as not standing and its on-foot task ignores the enter-vehicle
		// press meanwhile (the vehicle key's press was lost; a second F then worked). A small gap
		// is closed here: the root goes 2 cm below its resting height above the ground (GTA counts
		// the ped as standing at once and its physics lifts it the 2 cm). A larger gap is a real
		// jump or fall: left to GTA.
		void SettleOnGround(int a_ped)
		{
			if (S::IS_CHAR_IN_ANY_CAR(a_ped) || S::IS_CHAR_DEAD(a_ped)) {
				return;
			}
			float x = 0, y = 0, z = 0, ground = 0;
			S::GET_CHAR_COORDINATES(a_ped, &x, &y, &z);
			S::GET_GROUND_Z_FOR_3D_COORD(x, y, z, &ground);
			if (ground == 0.0f) {
				return;  // nothing found below
			}
			const float rest = (measured ? rootToFeet : 1.0f) - 0.02f;
			const float gap = z - ground - rest;
			if (gap > 0.01f && gap < 0.3f) {  // (a kerb is ~0.15 m; Minecraft slabs and stair steps, 0.5, stay)
				MovePed(a_ped, x, y, ground + rest, false);
				LC_LOG("puppet OFF: the ped was %.2f m above its resting height; set down onto the ground (z %.2f)", gap, ground);
			}
		}

		// ---- root -> feet ---------------------------------------------------------------------------------
		// GET_CHAR_COORDINATES is the ped's root (pelvis), about 1 m above the soles; Minecraft's
		// position is the feet. Measured while the player stands on the ground: root z minus
		// GET_GROUND_Z_FOR_3D_COORD from the root (GET_CHAR_HEIGHT_ABOVE_GROUND logged alongside).
		void MeasureRootToFeet(int a_ped, float a_x, float a_y, float a_z)
		{
			if (measured || !Cfg().measureRootToFeet) {
				return;
			}
			float vx = 0, vy = 0, vz = 0;
			S::GET_CHAR_VELOCITY(a_ped, &vx, &vy, &vz);
			if (std::fabs(vz) > 0.05f || vx * vx + vy * vy > 0.01f || S::IS_CHAR_IN_WATER(a_ped)) {
				return;
			}
			float ground = 0.0f, hag = 0.0f;
			S::GET_GROUND_Z_FOR_3D_COORD(a_x, a_y, a_z, &ground);
			S::GET_CHAR_HEIGHT_ABOVE_GROUND(a_ped, &hag);
			const float off = a_z - ground;
			if (ground == 0.0f || off < 0.4f || off > 1.6f) {
				return;
			}
			measureSum += off;
			measureHagSum += hag;
			if (++measureCount >= 30) {
				measured = true;
				rootToFeet = static_cast<float>(measureSum / measureCount);
				LC_LOG("root->feet measured: %.3f m (root z - ground z over %d still frames; GET_CHAR_HEIGHT_ABOVE_GROUND averaged %.3f; ini RootToFeet=%.2f)",
					rootToFeet, measureCount, measureHagSum / measureCount, Cfg().rootToFeet);
			}
		}

		// ---- camera rows ------------------------------------------------------------------------------------
		bool ParseRows(const std::string& a_text, Rows& a_out)
		{
			int n = 0;
			for (std::size_t i = 0; i < a_text.size() && n < 3; ++i) {
				float sign = 1.0f;
				if (a_text[i] == '-') {
					sign = -1.0f;
					++i;
				}
				if (i < a_text.size() && a_text[i] >= '0' && a_text[i] <= '2') {
					a_out.idx[n] = a_text[i] - '0';
					a_out.sign[n] = sign;
					++n;
					while (i + 1 < a_text.size() && a_text[i + 1] != ',') {
						++i;
					}
					++i;  // the comma
				}
			}
			a_out.known = n == 3;
			return a_out.known;
		}

		// Discovers which rows of the final camera's matrix are right / forward / up while the game's
		// own on-foot camera follows the player: forward points from the camera at the player,
		// up has the largest world z, right completes forward x up. 60 agreeing frames settle it.
		void DiscoverRows(const float* a_m)
		{
			CPed* ped = FindPlayerPed();
			if (!ped || !ped->m_pMatrix || !onFootForCamera) {
				return;
			}
			const auto& pp = ped->m_pMatrix->pos;
			float       d[3] = { pp.x - a_m[12], pp.y - a_m[13], pp.z + 0.5f - a_m[14] };
			const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			if (len < 1.0f || len > 30.0f) {
				return;
			}
			for (float& c : d) {
				c /= len;
			}
			float dots[3];
			for (int r = 0; r < 3; ++r) {
				dots[r] = a_m[r * 4] * d[0] + a_m[r * 4 + 1] * d[1] + a_m[r * 4 + 2] * d[2];
			}
			int fwd = 0;
			for (int r = 1; r < 3; ++r) {
				if (std::fabs(dots[r]) > std::fabs(dots[fwd])) {
					fwd = r;
				}
			}
			const int a = (fwd + 1) % 3, b = (fwd + 2) % 3;
			const int up = std::fabs(a_m[a * 4 + 2]) >= std::fabs(a_m[b * 4 + 2]) ? a : b;
			const int right = up == a ? b : a;
			if (std::fabs(dots[fwd]) < 0.85f || std::fabs(a_m[up * 4 + 2]) < 0.6f) {
				rowsVotes = 0;
				return;
			}
			Rows cand;
			cand.idx[0] = right;
			cand.idx[1] = fwd;
			cand.idx[2] = up;
			cand.sign[1] = dots[fwd] > 0.0f ? 1.0f : -1.0f;
			cand.sign[2] = a_m[up * 4 + 2] > 0.0f ? 1.0f : -1.0f;
			float F[3], U[3];
			for (int c = 0; c < 3; ++c) {
				F[c] = a_m[fwd * 4 + c] * cand.sign[1];
				U[c] = a_m[up * 4 + c] * cand.sign[2];
			}
			const float FxU[3] = { F[1] * U[2] - F[2] * U[1], F[2] * U[0] - F[0] * U[2], F[0] * U[1] - F[1] * U[0] };
			const float rd = a_m[right * 4] * FxU[0] + a_m[right * 4 + 1] * FxU[1] + a_m[right * 4 + 2] * FxU[2];
			cand.sign[0] = rd > 0.0f ? 1.0f : -1.0f;
			cand.known = true;
			if (rowsVotes > 0 && cand == rowsCandidate) {
				++rowsVotes;
			} else {
				rowsCandidate = cand;
				rowsVotes = 1;
			}
			if (rowsVotes >= 60 && rowsPinned) {
				rowsChecked = true;
				LC_LOG("camera rows check: the game's on-foot camera %s the pinned rows (right, forward, up = CMatrix rows %s%d,%s%d,%s%d)%s",
					rowsCandidate == rows ? "agrees with" : "DISAGREES with", rowsCandidate.sign[0] < 0 ? "-" : "", rowsCandidate.idx[0],
					rowsCandidate.sign[1] < 0 ? "-" : "", rowsCandidate.idx[1], rowsCandidate.sign[2] < 0 ? "-" : "", rowsCandidate.idx[2],
					rowsCandidate == rows ? "" : "; keeping the pinned ones (CameraRows=discover adopts what the game says)");
			} else if (rowsVotes >= 60) {
				rows = rowsCandidate;
				static const char* kNames[3] = { "right", "up", "at" };
				LC_LOG("camera rows discovered: camera right = %s%s, forward = %s%s, up = %s%s (CMatrix rows; |dot to player| %.2f). Pin with CameraRows=%s%d,%s%d,%s%d",
					rows.sign[0] < 0 ? "-" : "", kNames[rows.idx[0]], rows.sign[1] < 0 ? "-" : "", kNames[rows.idx[1]], rows.sign[2] < 0 ? "-" : "",
					kNames[rows.idx[2]], std::fabs(dots[fwd]), rows.sign[0] < 0 ? "-" : "", rows.idx[0], rows.sign[1] < 0 ? "-" : "", rows.idx[1],
					rows.sign[2] < 0 ? "-" : "", rows.idx[2]);
			}
		}

		void LogStats(std::uint64_t a_now, bool a_haveMc)
		{
			const bool  diag = Cfg().diagnostics;
			const auto  everyMs = diag ? 1000ull : 10000ull;
			if (lastStatsMs == 0) {
				lastStatsMs = a_now;
				return;
			}
			if (a_now - lastStatsMs < everyMs) {
				return;
			}
			const double secs = double(a_now - lastStatsMs) / 1000.0;
			lastStatsMs = a_now;
			const auto in = Input::TakeCounters();
			const auto col = Collision::Get().TakeCounters();
			LC_LOG("stats %.0fs: frames %.1f/s, SkyState writes %.1f/s (teleport #%u, acked #%u), McState reads %u fail %u, MC frame %llu flags 0x%X, "
				   "puppet %d, mc alive %d (heartbeat %llu ms)",
				secs, stats.frames / secs, stats.skyWrites / secs, teleportSeq, mc.teleportAck, stats.mcReads, stats.mcReadFails,
				static_cast<unsigned long long>(mc.frameCounter), mc.flags, puppeting, a_haveMc,
				static_cast<unsigned long long>(Link::Get().McHeartbeatAgeMs() == UINT64_MAX ? 0 : Link::Get().McHeartbeatAgeMs()));
			LC_LOG("stats %.0fs: input keys %u buttons %u scroll %u text %u cursor %u raw-mouse %u (retaken %u) releaseAll %u openMenu %u dropped %u pad-zeroed %u; events %u; camera writes %u",
				secs, in.keys, in.buttons, in.scrolls, in.chars, in.cursors, in.rawMouse, in.rawRetaken, in.releaseAll, in.openMenu, in.dropped, in.padZeroed, stats.events,
				stats.cameraWrites);
			LC_LOG("stats %.0fs: collision regions %u (tris %u, blocks %u), columns probed %u (not loaded %u, collision requests %u), clears %u, queued %u, "
				   "ring waits %u dropped %u, ring pending %llu KiB",
				secs, col.regions, col.tris, col.blocks, col.columns, col.columnFailures, col.collisionRequests, col.clears, col.queued, col.ringWaits,
				col.dropped, static_cast<unsigned long long>(col.ringPending >> 10));
			if (motion.frames) {
				LC_LOG("stats %.0fs: motion %u frames, %u MC ticks, %u late frames, frame avg %.1f ms max %.1f ms, render delay %.1f ms, tick stamp error avg %.2f ms, "
					   "ped off its spot by up to %.3f m",
					secs, motion.frames, motion.ticks, motion.lateFrames, motion.frameMsSum / motion.frames, motion.frameMsMax, renderDelayMs,
					motion.stampSamples ? motion.stampErrMs / motion.stampSamples : 0.0, moveDriftMax);
			}
			moveDriftMax = 0.0;
			stats = {};
			motion = {};
		}
	}

	Shared& State()
	{
		return shared;
	}

	void PlacePed(int a_ped, float a_x, float a_y, float a_z)
	{
		MovePed(a_ped, a_x, a_y, a_z, Cfg().puppetMove == "native");
	}

	void ReportHurt(std::uint16_t a_kind, float a_damage, std::uint32_t a_attacker, std::uint32_t a_flags)
	{
		// Combat calls this when GTA damages the player while puppeting.
		Link::Get().PushInput(proto::kInHurt, a_kind, static_cast<std::int32_t>(a_damage * 100.0f), static_cast<std::int32_t>(a_attacker),
			static_cast<std::int32_t>(a_flags));
	}

	void OnIngameStartup()
	{
		// A save / new game / episode is about to load: the ped and our cameras are going away.
		LC_LOG("game (re)loading: dropping puppet state");
		LeavePuppet("the game is loading a save", false);
		Combat::OnIngameStartup();
		HostDrive::OnIngameStartup();
		ViewportRoom::OnIngameStartup();
		BlockyCity::OnIngameStartup();
		teleportPending = true;
		haveLastSet = false;
		measured = false;
		measureCount = 0;
		measureSum = measureHagSum = 0.0;
		tickHistory.clear();
		ResetCollision("game load");
	}

	void Tick()
	{
		Perf::Report(Cfg().logPerf, Cfg().diagnostics);
		Perf::Scope timer(Perf::kTick);
		auto& link = Link::Get();
		link.StartHeartbeatThread();
		if (!link.Valid()) {
			link.Create();
		}
		Input::Install();
		shared.linkReady = link.Valid();
		if (!link.Valid()) {
			return;
		}
		++stats.frames;
		link.Heartbeat();
		const auto  nowMs = ::GetTickCount64();
		static auto lastQpc = Qpc();
		const auto  qpcNow = Qpc();
		const float dt = std::clamp(static_cast<float>(double(qpcNow - lastQpc) / QpcPerMs() / 1000.0), 0.0f, 0.1f);
		lastQpc = qpcNow;
		Input::Tick(dt);
		if (!measured) {
			rootToFeet = Cfg().rootToFeet;
		}

		// ---- Minecraft ------------------------------------------------------------------------------
		if (link.Generation() != linkGeneration) {
			if (linkGeneration != 0) {
				LC_LOG("link was recreated (generation %u): treating it as a reconnect", link.Generation());
				mcWasAlive = false;
				lastMcPid = 0;
			}
			linkGeneration = link.Generation();
		}
		const bool mcAlive = link.MinecraftAlive();
		// A single failed seqlock read (Minecraft mid-write, a GC pause, a slow Python stand-in) must
		// not drop puppet mode: that releases Niko's protections for a frame (seen in game: a
		// Minecraft explosion killed Niko 130 ms after such a flicker). Read into a scratch copy so a
		// torn read never reaches `mc`, and keep using the last clean state for a short grace period.
		constexpr std::uint64_t kMcStaleGraceMs = 250;
		bool       haveMc = false;
		if (mcAlive) {
			proto::McState fresh{};
			const bool ok = link.ReadMcState(fresh);
			++(ok ? stats.mcReads : stats.mcReadFails);
			if (ok) {
				mc = fresh;
				mcGoodMs = nowMs;
			}
			haveMc = mcGoodMs != 0 && nowMs - mcGoodMs <= kMcStaleGraceMs;
		} else {
			mcGoodMs = 0;
		}
		const auto mcPid = link.McPid();
		const bool newMcProcess = mcAlive && mcPid != 0 && mcPid != lastMcPid;
		if (mcAlive && (!mcWasAlive || newMcProcess)) {
			LC_LOG("Minecraft connected: pid %u, heartbeat %llu ms old, MC frame %llu, flags 0x%X", mcPid,
				static_cast<unsigned long long>(link.McHeartbeatAgeMs()), static_cast<unsigned long long>(mc.frameCounter), mc.flags);
			link.ResetOverlay();
			ResetCollision("Minecraft connected");
			teleportPending = true;
			tickHistory.clear();
		} else if (!mcAlive && mcWasAlive) {
			LC_LOG("Minecraft gone (heartbeat %llu ms old)", static_cast<unsigned long long>(link.McHeartbeatAgeMs()));
		}
		if (mcAlive) {
			lastMcPid = mcPid;
		}
		mcWasAlive = mcAlive;
		const bool mcInWorld = haveMc && (mc.flags & proto::kMcInWorld);
		const bool mcCity = haveMc && (mc.flags & proto::kMcBlockyCity);  // the blocky city (BlockyCity.h)
		const bool screenOpen = haveMc && (mc.flags & proto::kMcScreenOpen);
		if (screenOpen && !wasScreenOpen) {
			shared.cursorX = shared.viewportW / 2;
			shared.cursorY = shared.viewportH / 2;
		}
		wasScreenOpen = screenOpen;
		shared.mcScreenOpen = screenOpen;
		if (haveMc && mc.sensitivity > 0.0f) {
			shared.sensitivity = mc.sensitivity;
		}
		if (haveMc && mc.teleportAck == teleportSeq && lastAckLogged != teleportSeq) {
			lastAckLogged = teleportSeq;
			LC_LOG("Minecraft acknowledged teleport #%u (now at MC %.2f %.2f %.2f)", teleportSeq, mc.x, mc.y, mc.z);
		}

		// ---- GTA ------------------------------------------------------------------------------------
		const int  player = static_cast<int>(S::GET_PLAYER_ID());
		const bool playing = S::IS_PLAYER_PLAYING(player);
		int        ped = 0;
		if (playing) {
			S::GET_PLAYER_CHAR(player, &ped);
		}
		const bool exists = ped != 0 && S::DOES_CHAR_EXIST(ped);
		const bool paused = S::IS_PAUSE_MENU_ACTIVE();
		const bool fadedIn = S::IS_SCREEN_FADED_IN();
		const bool loading = !fadedIn || !exists;
		const bool inGame = exists && !loading && !paused;
		const bool dead = exists && S::IS_CHAR_DEAD(ped);
		const bool inCar = exists && S::IS_CHAR_IN_ANY_CAR(ped);
		const bool cutscene = CCutsceneMgr::IsRunning();
		const bool menuOpen = paused || loading;
		if (menuOpen != wasMenuOpen) {
			LC_LOG("GTA %s (pause menu %d, faded in %d, player %d)", menuOpen ? "menu/loading: input goes to GTA" : "back in game", paused, fadedIn, exists);
			if (menuOpen) {
				Input::ReleaseAll();
			}
			wasMenuOpen = menuOpen;
		}
		shared.gtaMenuOpen = menuOpen;
		onFootForCamera = inGame && !inCar && !cutscene && !dead;

		GtaVec feet{};
		float  heading = 0.0f;
		if (exists) {
			float x = 0, y = 0, z = 0;
			S::GET_CHAR_COORDINATES(ped, &x, &y, &z);
			S::GET_CHAR_HEADING(ped, &heading);
			if (!puppeting && inGame && !inCar) {
				MeasureRootToFeet(ped, x, y, z);
			}
			feet = { x, y, z - rootToFeet };
		}
		// ---- the blocky city: GTA's map geometry hidden while Minecraft's player is in it -------------------
		BlockyCity::Tick(mcCity && mcAlive && !loading, dt);
		const McVec feetMc = GtaToMc(feet);

		// ---- who drives: Minecraft, or GTA IV (Niko mode, vehicles, cutscenes; HostDrive.h) -------------
		HostDrive::Frame driveFrame;
		driveFrame.player = player;
		driveFrame.ped = exists ? ped : 0;
		driveFrame.exists = exists;
		driveFrame.loading = loading;
		driveFrame.paused = paused;
		driveFrame.dead = dead;
		driveFrame.inCar = inCar;
		driveFrame.cutscene = cutscene;
		driveFrame.puppeting = puppeting;
		driveFrame.mcInWorld = mcInWorld;
		driveFrame.dt = dt;
		driveFrame.heading = heading;
		driveFrame.resyncing = mcInWorld && !loading && exists && mc.teleportAck != teleportSeq;
		const HostDrive::Result drive = HostDrive::Tick(driveFrame);
		if (drive.resync) {
			LC_LOG("GTA IV let go of the player; teleporting Minecraft to them before it takes over");
			teleportPending = true;
		}
		if (drive.hostDrives) {
			// Minecraft's player faces where Niko (or the vehicle) does, and picks up from there.
			yaw = GtaHeadingToMcYaw(drive.heading);
			pitch = 0.0f;
			lookInit = true;
		}

		// World identity: 0 outdoors, else the interior handle (debounced: doorways flicker).
		if (exists) {
			int interior = 0;
			S::GET_INTERIOR_FROM_CHAR(ped, &interior);
			const auto id = static_cast<std::uint32_t>(interior);
			if (id != interiorCandidate) {
				interiorCandidate = id;
				interiorTimer = 0.0f;
			} else if (id != worldId && (interiorTimer += dt) >= kInteriorDebounce) {
				// GTA IV's interiors share the outdoors' coordinates (1 block = 1 m, no origin change): the
				// player walked through a door, nothing moved him. No teleport handshake (puppet mode goes
				// on) and no new collision epoch (Minecraft keeps the floor under him): the columns around
				// him are probed again and sent where they changed. A real jump (GTA warping him) is
				// caught below as the game moving the player.
				LC_LOG("world changed %u -> %u (%s): collision refreshed, puppet mode goes on", worldId, id, id ? "interior" : "outdoors");
				worldId = id;
				Collision::Get().Refresh();
			}
		}

		// The game moved the player itself (mission script, respawn, loading a save).
		if (loading) {
			teleportPending = true;
			haveLastSet = false;
		} else if (haveLastSet) {
			const double dx = feet.x - lastSetFeet.x, dy = feet.y - lastSetFeet.y, dz = feet.z - lastSetFeet.z;
			const double gap = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (gap > kGameTeleportMetres) {
				LC_LOG("the game moved the player %.1f m; resyncing Minecraft", gap);
				teleportPending = true;
				haveLastSet = false;
				settleTimer = kSettleSeconds;
			}
		}
		if (teleportPending && !loading && exists) {
			++teleportSeq;
			teleportPending = false;
			yaw = GtaHeadingToMcYaw(heading);
			pitch = 0.0f;
			lookInit = true;
			LC_LOG("teleport #%u: Minecraft to MC %.2f %.2f %.2f (GTA %.2f %.2f %.2f), yaw %.1f (GTA heading %.1f)", teleportSeq, feetMc.x, feetMc.y, feetMc.z,
				feet.x, feet.y, feet.z, yaw, heading);
		}

		// Mouse look with Minecraft's formula, integrated here so the camera has no added latency.
		float lookDx = 0.0f, lookDy = 0.0f;
		Input::ConsumeLook(lookDx, lookDy);
		if (!lookInit && exists) {
			yaw = GtaHeadingToMcYaw(heading);
			pitch = 0.0f;
			lookInit = true;
		}
		if (puppeting && !screenOpen && !menuOpen) {
			const float s = shared.sensitivity.load() * 0.6f + 0.2f;
			const float factor = s * s * s * 8.0f * 0.15f;
			yaw = std::fmod(yaw + lookDx * factor, 360.0f);
			pitch = std::clamp(pitch + lookDy * factor, -90.0f, 90.0f);
		}

		// Minecraft holds its player after a teleport until our ground has arrived around them. If it's
		// waiting somewhere our player isn't, that never happens: send it again.
		const bool arriving = mcInWorld && !loading && mc.teleportAck != teleportSeq;
		if (arriving) {
			const double gx = feetMc.x - mc.x, gy = feetMc.y - mc.y, gz = feetMc.z - mc.z;
			const double gap = std::sqrt(gx * gx + gy * gy + gz * gz);
			holdMismatch = gap > 8.0 ? holdMismatch + dt : 0.0f;
			if (holdMismatch > 1.0f) {
				LC_LOG("Minecraft is waiting %.0f blocks from the player; teleporting it again", gap);
				teleportPending = true;
				holdMismatch = 0.0f;
			}
		} else {
			holdMismatch = 0.0f;
		}

		// ---- puppet decision ------------------------------------------------------------------------
		const char* blocker = !Cfg().puppet           ? "Puppet=0 in LibertyCraft.ini"
		                      : !mcAlive              ? "Minecraft not running"
		                      : !mcInWorld            ? "Minecraft not in a world"
		                      : !exists               ? "no player ped"
		                      : loading               ? "loading / screen faded"
		                      : dead                  ? "player dead"
		                      : drive.blocker         ? drive.blocker
		                      : inCar                 ? "player in a vehicle"
		                      : cutscene              ? "cutscene"
		                      : mc.teleportAck != teleportSeq ? "waiting for Minecraft to acknowledge the teleport"
		                                                      : nullptr;
		if (paused && puppeting && exists) {
			blocker = nullptr;  // the pause menu doesn't end puppet mode
		}
		const bool want = blocker == nullptr;
		if (blocker != lastBlocker && blocker && (Cfg().diagnostics || !puppeting)) {
			LC_LOG_EVERY(2000, "not puppeting: %s", blocker);
		}
		lastBlocker = blocker;
		if (want && !puppeting) {
			EnterPuppet(player, ped, feet);
		} else if (!want && puppeting) {
			// The teleport handshake (the game moved the player): Niko stays hidden, HostDrive puts the
			// Minecraft body on him until Minecraft has arrived and puppet mode has him again.
			LeavePuppet(blocker, true, mc.teleportAck != teleportSeq && blocker && std::strcmp(blocker, "waiting for Minecraft to acknowledge the teleport") == 0 && !shared.nikoMode);
		} else if (puppeting && ped != puppetPed) {
			LeavePuppet("the player ped changed");
		}
		HostDrive::AfterPuppetDecision();
		if (holsteredWeapon && !puppeting && shared.nikoMode && exists && !dead) {
			S::SET_CURRENT_CHAR_WEAPON(ped, holsteredWeapon, true);
			LC_LOG("Niko mode: GTA weapon %u back in Niko's hand", holsteredWeapon);
			holsteredWeapon = 0;
		}
		// In a vehicle in Minecraft mode GTA's weapon stays put away too, and there are no drive-bys;
		// Niko mode gives both back. In a vehicle GTA's HUD shows the weapon it would drive-by with (the
		// pistol, with the current weapon unarmed: measured), next to Minecraft's hotbar: its weapon icon
		// and ammo are hidden meanwhile (SET_HIDE_WEAPON_ICON / DISPLAY_AMMO set flags that stay; set again
		// every 0.5 s in case a script changes them).
		{
			static bool  driveByOff = false;
			static float hudT = 0.0f;
			const bool   keepAway = exists && !dead && !puppeting && !shared.nikoMode && (inCar || drive.inVehicle);
			unsigned     weapon = 0;
			if (keepAway && S::GET_CURRENT_CHAR_WEAPON(ped, &weapon) && weapon != WEAPON_UNARMED) {
				holsteredWeapon = weapon;
				S::SET_CURRENT_CHAR_WEAPON(ped, WEAPON_UNARMED, true);
				LC_LOG_EVERY(2000, "GTA weapon %u put away in the vehicle (Minecraft mode)", weapon);
			}
			if (keepAway != driveByOff && exists) {
				driveByOff = keepAway;
				S::SET_PLAYER_CAN_DO_DRIVE_BY(player, !keepAway);
				S::SET_HIDE_WEAPON_ICON(keepAway);
				S::DISPLAY_AMMO(!keepAway);
				hudT = 0.5f;
				LC_LOG("drive-bys and GTA's weapon icon %s", keepAway ? "off (Minecraft mode in a vehicle)" : "back");
			} else if (keepAway && (hudT -= dt) <= 0.0f) {
				hudT = 0.5f;
				S::SET_HIDE_WEAPON_ICON(true);
				S::DISPLAY_AMMO(false);
			}
		}

		if (puppeting) {
			const Pose pose = Interpolate();
			GtaVec    target = McToGta(pose.feet);
			const int moveTest = HostDrive::DebugPuppetTarget(target);  // DebugWalkThroughCar: 1 native, 2 direct
			if (!paused) {
				const float px = static_cast<float>(target.x), py = static_cast<float>(target.y), pz = static_cast<float>(target.z + rootToFeet);
				if (CPed* obj = FindPlayerPed(); obj && obj->m_pMatrix && haveMoveCheck) {
					const auto&  m = obj->m_pMatrix->pos;
					const double dx = m.x - moveCheck[0], dy = m.y - moveCheck[1], dz = m.z - moveCheck[2];
					moveDriftMax = std::max(moveDriftMax, std::sqrt(dx * dx + dy * dy + dz * dz));
				}
				MovePed(ped, px, py, pz, moveTest == 1 || (moveTest == 0 && Cfg().puppetMove == "native"));
				moveCheck[0] = px, moveCheck[1] = py, moveCheck[2] = pz;
				haveMoveCheck = true;
				S::SET_CHAR_HEADING(ped, McYawToGtaHeading(yaw));
				if (!Cfg().freezePed) {
					S::SET_CHAR_VELOCITY(ped, 0.0f, 0.0f, 0.0f);
				}
			}
			lastSetFeet = target;
			haveLastSet = true;
			BuildCamPose(pose, dt);
			ViewportRoom::Tick(camPose.pos, ped, true, zoom > 0.05f, dt);  // (interiors: GTA renders from the camera's room)
			gtaCameraRoomCheck = 1.5f;
			// Niko stays hidden in every Minecraft camera mode: first person the camera sits in his head,
			// third person Minecraft's own body is drawn there (his limbs showed through it). Every frame:
			// GTA (scripts, the phone) can show him again.
			S::SET_CHAR_VISIBLE(ped, false);
			if (pedHidden != 1) {
				pedHidden = 1;
				LC_LOG("Niko hidden while puppeting (camera mode %d)", mc.cameraMode);
			}
			if ((reassertTimer -= dt) <= 0.0f) {
				reassertTimer = kReassertSeconds;
				ApplyPuppetState(player, ped);
			}
			if (Cfg().cameraMode == Config::CameraMode::kScripted) {
				if (!scriptCam) {
					S::CREATE_CAM(14, &scriptCam);
					S::SET_CAM_ACTIVE(scriptCam, true);
					S::ACTIVATE_SCRIPTED_CAMS(true, true);
					LC_LOG("scripted camera %d created", scriptCam);
				}
				if (scriptCam) {
					S::SET_CAM_POS(scriptCam, camPose.pos[0], camPose.pos[1], camPose.pos[2]);
					S::SET_CAM_ROT(scriptCam, McPitchToGtaPitch(camPose.pitch), 0.0f, McYawToGtaHeading(camPose.yaw));
					S::SET_CAM_FOV(scriptCam, GtaFov(camPose.fovDeg));
				}
			}
		} else {
			// GTA's camera jumps back to Niko: the room it renders from is checked for a moment longer.
			if (gtaCameraRoomCheck > 0.0f && exists && TheCamera.m_pFinalCam) {
				gtaCameraRoomCheck -= dt;
				const auto& c = TheCamera.m_pFinalCam->m_mMatrix.pos;
				const float at[3] = { c.x, c.y, c.z };
				ViewportRoom::Tick(at, ped, false, true, dt);
			}
			camPose.valid = false;
			tickHistory.clear();
			lastFrameQpc = 0;
			haveMoveCheck = false;
		}

		// ---- GTA's doors swing open for the Minecraft player (Doors.h) ------------------------------------
		{
			const GtaVec at = puppeting && haveLastSet ? lastSetFeet : feet;
			Doors::Frame df;
			df.ped = exists ? ped : 0;
			df.loading = loading;
			df.puppeting = puppeting && !paused;
			df.feet[0] = static_cast<float>(at.x);
			df.feet[1] = static_cast<float>(at.y);
			df.feet[2] = static_cast<float>(at.z);
			df.dt = dt;
			Doors::Tick(df);
		}

		// ---- tell Minecraft where the player is and where they look -----------------------------------
		proto::SkyState sky{};
		sky.flags = (inGame ? proto::kSkyInGame : 0u) | (paused ? proto::kSkyMenuOpen : 0u) | (loading ? proto::kSkyLoading : 0u)
		          | (drive.hostDrives ? proto::kSkyHostDrives : 0u) | (drive.inVehicle ? proto::kSkyInVehicle : 0u);
		sky.worldId = worldId;
		sky.collisionEpoch = epoch;
		const McVec skyPos = drive.inVehicle ? GtaToMc(drive.seatFeet) : feetMc;  // in a vehicle: the rider's feet
		sky.posX = skyPos.x;
		sky.posY = skyPos.y;
		sky.posZ = skyPos.z;
		sky.yaw = yaw;
		sky.pitch = pitch;
		sky.teleportSeq = teleportSeq;
		int vw = shared.viewportW, vh = shared.viewportH;
		if (vw <= 0 || vh <= 0) {
			S::GET_SCREEN_RESOLUTION(&vw, &vh);
		}
		sky.viewportW = static_cast<std::uint32_t>(std::max(vw, 0));
		sky.viewportH = static_cast<std::uint32_t>(std::max(vh, 0));
		int hour = 0, minute = 0;
		S::GET_TIME_OF_DAY(&hour, &minute);
		sky.gameHour = static_cast<float>(hour) + static_cast<float>(minute) / 60.0f;
		link.WriteSkyState(sky);
		++stats.skyWrites;
		if (!firstSkyWriteLogged) {
			firstSkyWriteLogged = true;
			LC_LOG("first SkyState written: flags 0x%X, MC pos %.2f %.2f %.2f, yaw %.1f, viewport %ux%u, hour %.2f", sky.flags, sky.posX, sky.posY, sky.posZ,
				sky.yaw, sky.viewportW, sky.viewportH, sky.gameHour);
		}

		// ---- combat: the actor table, Minecraft's events, GTA's damage to the puppeted player ------------
		{
			Combat::Frame cf;
			cf.player = player;
			cf.ped = exists ? ped : 0;
			cf.exists = exists;
			cf.loading = loading;
			cf.dead = dead;
			cf.puppeting = puppeting || HostDrive::KnockedOver();  // knocked over: Minecraft still owns the player's health
			cf.mcInWorld = mcInWorld;
			cf.mc = haveMc ? &mc : nullptr;
			cf.dt = dt;
			stats.events += Combat::Tick(cf);
			Hazards::Tick(cf);  // Minecraft's fire, lava and magma burn GTA's peds
		}

		// ---- collision --------------------------------------------------------------------------------
		settleTimer -= dt;
		if (haveMc && inGame && settleTimer <= 0.0f) {
			Perf::Scope col(Perf::kCollision);
			Collision::Get().Update(puppeting ? McVec{ mc.x, mc.y, mc.z } : feetMc, static_cast<float>(feet.z));
		}

		LogStats(nowMs, haveMc);
	}

	void Camera()
	{
		Perf::Scope timer(Perf::kCamera);
		CCam* cam = TheCamera.m_pFinalCam;
		if (!cam) {
			return;
		}
		auto* m = reinterpret_cast<float*>(&cam->m_mMatrix);  // 4 rows of 4 floats: right, up, at, pos
		if (!rowsConfigured) {
			rowsConfigured = true;
			if (Cfg().cameraRows == "auto") {
				// Right, forward, up are CMatrix rows right, up, at (0, 1, 2): every session on 1.0.8.0
				// discovers exactly these. Pinned so the very first puppet frames never use rows that
				// are still being discovered (or were discovered from a transition camera).
				rows = Rows{};
				rows.known = true;
				rowsPinned = true;
			} else if (Cfg().cameraRows == "discover") {
				LC_LOG("camera rows: discovering them from the game's on-foot camera (CameraRows=discover)");
			} else {
				Rows parsed;
				if (ParseRows(Cfg().cameraRows, parsed)) {
					rows = parsed;
					LC_LOG("camera rows from LibertyCraft.ini: %s", Cfg().cameraRows.c_str());
				} else {
					LC_LOG("CameraRows=%s not understood (want e.g. 0,1,2 or -0,1,2); discovering them", Cfg().cameraRows.c_str());
				}
			}
		}
		if (puppeting && camPose.valid && Cfg().cameraMode == Config::CameraMode::kFinal &&
			std::isfinite(camPose.pos[0] + camPose.pos[1] + camPose.pos[2] + camPose.yaw + camPose.pitch)) {
			const GtaBasis b = LookBasis(camPose.yaw, camPose.pitch);
			const float*   axes[3] = { b.right, b.forward, b.up };
			for (int k = 0; k < 3; ++k) {
				float* row = m + rows.idx[k] * 4;
				for (int c = 0; c < 3; ++c) {
					row[c] = axes[k][c] * rows.sign[k];
				}
			}
			m[12] = camPose.pos[0];
			m[13] = camPose.pos[1];
			m[14] = camPose.pos[2];
			cam->m_fFOV = GtaFov(camPose.fovDeg);
			++stats.cameraWrites;
			return;
		}
		if ((!rows.known || (rowsPinned && !rowsChecked)) && !puppeting) {
			DiscoverRows(m);
		}
		if (Cfg().diagnostics) {
			LC_LOG_EVERY(5000, "final cam: right (%.2f %.2f %.2f) up (%.2f %.2f %.2f) at (%.2f %.2f %.2f) pos (%.1f %.1f %.1f) fov %.1f near %.2f far %.0f",
				m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14], cam->m_fFOV, cam->m_fNearZ, cam->m_fFarZ);
		}
	}
}

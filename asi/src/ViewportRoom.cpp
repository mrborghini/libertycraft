// Unity-built into dllmain.cpp (needs IV-SDK). See ViewportRoom.h.
//
// What the game does (1.0.8.0, read from its code):
//  - the viewport natives go through the viewport manager (0x11C6E58): each of the viewport's render
//    phases that scans the scene has a portal tracker (+0x30 its interior instance, null outside; +0x38
//    its room); GET_KEY_FOR_VIEWPORT_IN_ROOM gives -1 while the tracker is outside, else its room's key;
//  - CLEAR_ROOM_FOR_VIEWPORT takes the trackers out of their interior: outside;
//  - SET_ROOM_FOR_VIEWPORT_BY_KEY puts the trackers at the viewport's camera position (viewport +0x80)
//    and has them look for their room there (the game's own search after a teleport: interiors whose
//    bounds hold the point, then a probe for the room); with key 0 whichever room is there;
//  - GET_INTERIOR_AT_COORDS gives the interior whose bounds hold a point (0 outside them all).
// The camera outside every interior's bounds while the tracker is in one: it goes outside. A camera
// away from the player's eye (third person) in an interior's bounds (they reach metres out into the
// street): the game's own room search where it is, each time it has moved 0.25 m: room boxes, then a
// probe down for the room the collision below belongs to; nothing there: outside (0x91E8C0).
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "room"
#include "ViewportRoom.h"

#include "Config.h"
#include "Log.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace lc::ViewportRoom
{
	namespace
	{
		namespace S = ::Scripting;

		int           mismatch = 0;      // frames in a row the camera and the tracker disagree
		float         searchWait = 0.0f;  // seconds until the next room search may run
		float         searchedAt[3]{};
		bool          searched = false;
		float         debugLogT = 0.0f, debugToggleT = 0.0f;
		bool          debugFixOn = true;
		std::uint64_t nextStats = 0;
		struct Counters
		{
			std::uint32_t frames = 0, nearby = 0, clears = 0, searches = 0, changed = 0, moved = 0;
		} counters;

		// The game viewport object (1.0.8.0): GET_GAME_VIEWPORT_ID reads its id at +0x53C of the
		// object at 0x11F8290; its camera position is at +0x80. Checked once against the native's code.
		std::uint8_t* GameViewport(int a_id)
		{
			static int ok = -1;
			if (ok < 0) {
				static const std::uint8_t kCode[] = { 0xA1, 0x90, 0x82, 0x1F, 0x01, 0x8B, 0x88, 0x3C, 0x05, 0x00, 0x00 };
				ok = plugin::gameVer == plugin::VERSION_1080 &&
				             std::memcmp(reinterpret_cast<const void*>(AddressSetter::gBaseAddress + 0x75CDA0), kCode, sizeof(kCode)) == 0
				         ? 1
				         : 0;
				LC_LOG("game viewport object %s", ok ? "found (its camera position is read for the room search)" : "not where 1.0.8.0 has it (natives only)");
			}
			if (!ok) {
				return nullptr;
			}
			std::uint8_t* vp = nullptr;
			__try {
				vp = *reinterpret_cast<std::uint8_t**>(AddressSetter::gBaseAddress + 0xDF8290);
				if (vp && *reinterpret_cast<const int*>(vp + 0x53C) != a_id) {
					vp = nullptr;
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				vp = nullptr;
			}
			return vp;
		}

		bool ViewportPos(std::uint8_t* a_vp, float a_out[3])
		{
			if (!a_vp) {
				return false;
			}
			__try {
				std::memcpy(a_out, a_vp + 0x80, sizeof(float) * 3);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
			return std::isfinite(a_out[0] + a_out[1] + a_out[2]);
		}

		bool SetViewportPos(std::uint8_t* a_vp, const float a_pos[3])
		{
			__try {
				std::memcpy(a_vp + 0x80, a_pos, sizeof(float) * 3);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
			return true;
		}

		void Stats()
		{
			const auto now = ::GetTickCount64();
			if (now < nextStats) {
				return;
			}
			nextStats = now + 10000;
			if (counters.clears || counters.searches || Config::Get().debugViewportRoom) {
				LC_LOG("stats 10s: %u frames near an interior: GTA's rendering sent outside %u times, room searches at the camera %u (%u changed the room; the "
					   "viewport's position moved to this frame's camera %u)",
					counters.nearby, counters.clears, counters.searches, counters.changed, counters.moved);
			}
			counters = {};
		}
	}

	void Tick(const float a_cam[3], int a_ped, bool a_ours, bool a_detached, float a_dt)
	{
		++counters.frames;
		Stats();
		if (!a_ped || !std::isfinite(a_cam[0] + a_cam[1] + a_cam[2])) {
			return;
		}
		int vp = 0;
		S::GET_GAME_VIEWPORT_ID(&vp);
		int trackerKey = -1;
		S::GET_KEY_FOR_VIEWPORT_IN_ROOM(vp, &trackerKey);
		int camInterior = 0;
		S::GET_INTERIOR_AT_COORDS(a_cam[0], a_cam[1], a_cam[2], &camInterior);
		searchWait -= a_dt;
		const int debug = Config::Get().debugViewportRoom;
		if (debug >= 2 && (debugToggleT -= a_dt) <= 0.0f) {
			debugToggleT = 8.0f;
			debugFixOn = !debugFixOn;
			LC_LOG("DebugViewportRoom: fix %s", debugFixOn ? "ON" : "OFF");
		}
		if (!camInterior && trackerKey == -1) {
			mismatch = 0;
			return;  // (outdoors, no interior near)
		}
		++counters.nearby;
		if (debug && (debugLogT -= a_dt) <= 0.0f) {
			debugLogT = 0.25f;
			int          pedInterior = 0;
			unsigned int pedKey = 0;
			S::GET_INTERIOR_FROM_CHAR(a_ped, &pedInterior);
			S::GET_KEY_FOR_CHAR_IN_ROOM(a_ped, &pedKey);
			float vpPos[3]{};
			const bool haveVp = ViewportPos(GameViewport(vp), vpPos);
			LC_LOG("DebugViewportRoom: %s camera%s %.2f %.2f %.2f in interior %d's bounds; viewport %d at %s%.2f %.2f %.2f, its room key %d (%s); player in interior "
				   "%d room %08X",
				a_ours ? "Minecraft's" : "GTA's", a_detached ? " (detached)" : "", a_cam[0], a_cam[1], a_cam[2], camInterior, vp, haveVp ? "" : "(unread) ", vpPos[0],
				vpPos[1], vpPos[2], trackerKey, trackerKey == -1 ? "outside" : trackerKey == 0 ? "the interior's room 0" : "a room", pedInterior, pedKey);
		}
		if (!debugFixOn) {
			return;
		}
		if (!camInterior) {
			// Outside every interior's bounds while GTA renders from one: certainly outside.
			if (++mismatch < 2) {
				return;  // (a frame for GTA's own tracker to cross the portal)
			}
			S::CLEAR_ROOM_FOR_VIEWPORT(vp);
			++counters.clears;
			LC_LOG_EVERY(2000, "the camera is outside every interior (%.1f %.1f %.1f) while GTA renders from one (key %d): rendering from outside", a_cam[0], a_cam[1],
				a_cam[2], trackerKey);
			mismatch = 0;
			return;
		}
		mismatch = 0;
		// In an interior's bounds (they reach metres out into the street): a camera away from the player's
		// eye (Minecraft's third person, GTA's own camera just after puppet mode) can be in another room
		// than the one the tracker followed it into, or outside. The game's own room search where the
		// camera is (room boxes, then a probe down for the room its collision belongs to; nothing there:
		// outside), each time the camera has moved 0.25 m (0.5 s at a slower pace).
		if (!a_detached) {
			return;
		}
		const float moved = searched ? std::sqrt((a_cam[0] - searchedAt[0]) * (a_cam[0] - searchedAt[0]) + (a_cam[1] - searchedAt[1]) * (a_cam[1] - searchedAt[1]) +
		                                        (a_cam[2] - searchedAt[2]) * (a_cam[2] - searchedAt[2]))
		                           : 1e9f;
		if (moved < 0.25f && (searchWait > 0.0f || moved < 0.02f)) {
			return;
		}
		searchWait = 0.5f;
		std::memcpy(searchedAt, a_cam, sizeof(searchedAt));
		searched = true;
		// The search runs at the viewport's camera position (last frame's camera: measured); this frame's.
		if (std::uint8_t* obj = a_ours ? GameViewport(vp) : nullptr) {
			float p[3];
			if (ViewportPos(obj, p) && std::fabs(p[0] - a_cam[0]) + std::fabs(p[1] - a_cam[1]) + std::fabs(p[2] - a_cam[2]) > 0.05f && SetViewportPos(obj, a_cam)) {
				++counters.moved;
			}
		}
		S::SET_ROOM_FOR_VIEWPORT_BY_KEY(vp, 0);
		++counters.searches;
		int after = -1;
		S::GET_KEY_FOR_VIEWPORT_IN_ROOM(vp, &after);
		if (after != trackerKey) {
			++counters.changed;
			LC_LOG_EVERY(1000, "the room search at the camera (%.1f %.1f %.1f, interior %d's bounds) put GTA's rendering from %s %d to %s %d", a_cam[0], a_cam[1], a_cam[2],
				camInterior, trackerKey == -1 ? "outside" : "room", trackerKey, after == -1 ? "outside" : "room", after);
		}
	}

	void OnIngameStartup()
	{
		mismatch = 0;
		searched = false;
		searchWait = 0.0f;
	}
}

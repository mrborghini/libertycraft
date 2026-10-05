// Unity-built into dllmain.cpp (needs IV-SDK). See NikoBody.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "body"
#include "NikoBody.h"

#include "render/Body.h"
#include "render/World.h"

#include "Config.h"
#include "Input.h"
#include "Log.h"
#include "Missions.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace lc::NikoBody
{
	namespace
	{
		namespace S = ::Scripting;
		namespace B = render::body;

		// GTA IV's ped bone ids (IV-SDK's eBone).
		enum : unsigned
		{
			kBonePelvis = 0x1A1,
			kBoneLThigh = 0x1A2,
			kBoneLCalf = 0x1A3,
			kBoneLFoot = 0x1A4,
			kBoneRThigh = 0x1A7,
			kBoneRCalf = 0x1A8,
			kBoneRFoot = 0x1A9,
			kBoneNeck = 0x4B4,
			kBoneHead = 0x4B5,
			kBoneLUpperArm = 0x4C1,
			kBoneLHand = 0x4C3,
			kBoneRUpperArm = 0x4C8,
			kBoneRHand = 0x4D0,
		};
		constexpr float kHeadProbe = 0.1f;   // GET_PED_BONE_POSITION offsets along the head bone's axes
		constexpr float kMaxReach = 2.5f;    // a bone further than this from the pelvis: a bad read
		constexpr int   kCalibrateFrames = 60;

		const char* WhyName(drive::Why a_why)
		{
			switch (a_why) {
			case drive::Why::kNikoMode:
				return "Niko mode";
			case drive::Why::kVehicle:
				return "vehicle";
			case drive::Why::kCutscene:
				return "cutscene";
			case drive::Why::kRagdoll:
				return "knocked over";
			case drive::Why::kScript:
				return "mission scene";
			default:
				return "none";
			}
		}

		int        target = 0;  // the ped Capture reads (Target)
		// In GTA IV's cutscenes Niko is no ped: the cutscene animates an object of the player's model
		// (object pool, with an animation player at +0x78). Its bones come from the game's own "bone
		// tag -> world matrix" function (the one GET_PED_BONE_POSITION uses; 1.0.8.0: 0x941E30) after
		// CDynamicEntity::GetBoneMatrix has waited for its pose job. The game only poses what it
		// draws, so it can't be hidden like a ped: invisible (SET_OBJECT_VISIBLE) it stops moving, and
		// an alpha of 0 doesn't hold (the game fades it back in by 16 a frame: a ghost of Niko over the
		// body). Instead its matrix is shrunk to kGhostScale every frame: the game still poses and
		// draws it, as a doll a few centimetres tall inside the Minecraft body, and its bones are
		// scaled back up about its position when read.
		CObject*   targetObj = nullptr;
		int        targetObjHandle = 0;
		bool       targetObjShown = false;  // the cutscene shows it this frame
		int        hiddenObj = 0;           // the object we shrank (handle)
		constexpr float kGhostScale = 0.05f;
		using BoneWorldFn = void(__thiscall*)(void* a_entity, float* a_out, int a_tag);
		BoneWorldFn boneWorld = nullptr;
		bool        boneWorldChecked = false;

		BoneWorldFn BoneWorld()
		{
			if (!boneWorldChecked) {
				boneWorldChecked = true;
				static const std::uint8_t kCode[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x06, 0x8B, 0x90, 0xA0, 0x00, 0x00, 0x00, 0xFF, 0xD2, 0x85, 0xC0, 0x74, 0x1A };
				const auto at = AddressSetter::gBaseAddress + 0x541E30;
				if (plugin::gameVer == plugin::VERSION_1080 && std::memcmp(reinterpret_cast<const void*>(at), kCode, sizeof(kCode)) == 0) {
					boneWorld = reinterpret_cast<BoneWorldFn>(at);
					LC_LOG("cutscene actors: the game's bone matrix function found at %08X", static_cast<unsigned>(at));
				} else {
					LC_LOG("WARNING: the game's bone matrix function isn't where 1.0.8.0 has it: no Minecraft body in cutscenes");
				}
			}
			return boneWorld;
		}
		drive::Why targetWhy = drive::Why::kNone;
		int        lastTarget = -1;
		drive::Why lastWhy = drive::Why::kNone;

		B::HeadMap headMap = B::kNikoHead;  // the head bone's axes (checked by CalibrateHead once)
		B::HeadMap candidate;
		int        candidateFrames = 0;
		bool       headChecked = false;
		bool       loggedFirst = false, loggedFirstObj = false;
		bool       worldOffsets = false;  // the bone offsets come back in world axes: no head rotation

		// drawingEvent runs twice per frame: the second call reuses the first's pose.
		std::uint32_t cachedFrame = 0xFFFFFFFF;
		bool          cachedOk = false;
		double        cachedOrigin[3]{};
		float         cachedParts[7][3][4]{};
		bool          cachedHideBack = false;

		// stats / DebugBody
		std::uint32_t frames = 0, failed = 0, headFrames = 0;
		std::uint32_t heldHiddenFrames = 0, cameraInsideFrames = 0;  // held items away (seated); parts hidden around the camera
		bool          lastSeated = false, lastPhone = false;
		float         seatedSmooth = 0.0f;  // B::SeatedAmount, eased
		std::uint64_t seatedAt = 0;
		std::uint32_t lastInside = 0;
		double        liftSum = 0.0, sinkSum = 0.0, limbMotion = 0.0;
		B::V3         lastLimbs[4]{};
		bool          haveLimbs = false;
		std::uint64_t nextStats = 0, nextDebug = 0, nextScanLog = 0;
		B::V3         prevPedPos{}, lastPedPos{};  // the target ped's position last frame (DebugBody)

		// A cutscene actor's bone (a_offset along the bone's own axes, like GET_PED_BONE_POSITION's).
		bool ReadObjectBone(CObject* a_obj, unsigned a_bone, float a_ox, float a_oy, float a_oz, B::V3& a_out)
		{
			const auto fn = BoneWorld();
			if (!fn || !a_obj) {
				return false;
			}
			alignas(16) float m[16]{};
			fn(a_obj, m, static_cast<int>(a_bone));
			// Undo the shrinking (ShrinkObject) about the object's position.
			const auto& em = *a_obj->m_pMatrix;
			const float k = std::sqrt(em.right.x * em.right.x + em.right.y * em.right.y + em.right.z * em.right.z);
			const float inv = k > 1e-3f ? 1.0f / k : 1.0f;
			const B::V3 o{ em.pos.x, em.pos.y, em.pos.z };
			const B::V3 p = o + (B::V3{ m[12], m[13], m[14] } - o) * inv;
			const B::V3 ax[3] = { B::V3{ m[0], m[1], m[2] } * inv, B::V3{ m[4], m[5], m[6] } * inv, B::V3{ m[8], m[9], m[10] } * inv };
			a_out = p + ax[0] * a_ox + ax[1] * a_oy + ax[2] * a_oz;
			return B::Finite(a_out) && (a_out.x != 0.0f || a_out.y != 0.0f || a_out.z != 0.0f);
		}

		bool ReadBone(int a_ped, unsigned a_bone, float a_ox, float a_oy, float a_oz, B::V3& a_out)
		{
			if (!a_ped) {
				return ReadObjectBone(targetObj, a_bone, a_ox, a_oy, a_oz, a_out);
			}
			Scripting::Vector3 v{};
			S::GET_PED_BONE_POSITION(a_ped, a_bone, a_ox, a_oy, a_oz, &v);
			a_out = { v.x, v.y, v.z };
			return B::Finite(a_out) && (v.x != 0.0f || v.y != 0.0f || v.z != 0.0f);
		}

		// DebugBody, in cutscenes: the animated objects around the camera (GTA IV's cutscene actors
		// aren't peds), nearest first.
		void LogCutsceneObjects(CPed* a_player, float a_cx, float a_cy, float a_cz)
		{
			auto* pool = CPools::ms_pObjectPool;
			if (!pool) {
				return;
			}
			auto hashOf = [](int a_index) -> unsigned {
				CBaseModelInfo* mi = a_index >= 0 && a_index < 31000 ? CModelInfo::ms_modelInfoPtrs[a_index] : nullptr;
				return mi ? mi->m_nHash : 0u;
			};
			std::vector<std::pair<float, CObject*>> found;
			for (int i = pool->FindNextUsed(0); i >= 0; i = pool->FindNextUsed(i + 1)) {
				CObject* o = pool->Get(i);
				if (!o || !o->m_pMatrix || !*reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(o) + 0x78)) {
					continue;
				}
				const auto& m = o->m_pMatrix->pos;
				const float d2 = (m.x - a_cx) * (m.x - a_cx) + (m.y - a_cy) * (m.y - a_cy) + (m.z - a_cz) * (m.z - a_cz);
				if (d2 < 40.0f * 40.0f) {
					found.emplace_back(d2, o);
				}
			}
			std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
			for (std::size_t k = 0; k < found.size() && k < 10; ++k) {
				CObject*    o = found[k].second;
				const auto& m = o->m_pMatrix->pos;
				const auto  flags = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(o) + 0x24);
				LC_LOG("  animated object %d model %d (hash %08X%s) %.1f m from the camera at %.1f %.1f %.1f, visible %d alpha %u", static_cast<int>(pool->GetIndex(o)),
					o->m_nModelIndex, hashOf(o->m_nModelIndex), o->m_nModelIndex == a_player->m_nModelIndex ? ", the player's model" : "", std::sqrt(found[k].first),
					m.x, m.y, m.z, (flags >> 5) & 1, o->m_nAlpha);
			}
			LC_LOG("  (%zu animated objects within 40 m of the camera; the player's model %d hash %08X)", found.size(), a_player->m_nModelIndex, hashOf(a_player->m_nModelIndex));
		}

		// The cutscene's Niko: an animated object of the player's model, nearest the camera.
		CObject* CutsceneActor(CPed* a_player, int& a_handle)
		{
			auto* pool = CPools::ms_pObjectPool;
			if (!pool || !a_player || !BoneWorld()) {
				return nullptr;
			}
			const auto& cam = TheCamera.m_pFinalCam ? TheCamera.m_pFinalCam->m_mMatrix.pos : a_player->m_pMatrix->pos;
			CObject*    best = nullptr;
			float       bestD2 = 1e30f;
			for (int i = pool->FindNextUsed(0); i >= 0; i = pool->FindNextUsed(i + 1)) {
				CObject* o = pool->Get(i);
				if (!o || !o->m_pMatrix || o->m_nModelIndex != a_player->m_nModelIndex ||
					!*reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(o) + 0x78)) {
					continue;
				}
				const auto& m = o->m_pMatrix->pos;
				const float d2 = (m.x - cam.x) * (m.x - cam.x) + (m.y - cam.y) * (m.y - cam.y) + (m.z - cam.z) * (m.z - cam.z);
				if (d2 < bestD2) {
					bestD2 = d2;
					best = o;
				}
			}
			a_handle = best ? static_cast<int>(pool->GetIndex(best)) : 0;
			return best;
		}

		// The cutscene actor's matrix rows scaled to a_scale (1: back to normal size).
		void ScaleObject(CObject* a_obj, float a_scale)
		{
			if (!a_obj || !a_obj->m_pMatrix) {
				return;
			}
			auto& m = *a_obj->m_pMatrix;
			const float k = std::sqrt(m.right.x * m.right.x + m.right.y * m.right.y + m.right.z * m.right.z);
			if (k < 1e-4f || std::fabs(k - a_scale) < 1e-3f) {
				return;
			}
			const float f = a_scale / k;
			for (CVector* r : { &m.right, &m.up, &m.at }) {
				r->x *= f;
				r->y *= f;
				r->z *= f;
			}
		}

		void ShowObject(int a_handle)
		{
			auto* pool = CPools::ms_pObjectPool;
			if (a_handle && pool && S::DOES_OBJECT_EXIST(a_handle)) {
				ScaleObject(pool->GetAt(static_cast<std::uint32_t>(a_handle)), 1.0f);
			}
		}

		// A Niko the cutscene animates: a ped of the player's model other than the player (the
		// cutscene's own actor), else the player ped itself while it is shown.
		int CutscenePed(int a_player)
		{
			CPed* player = FindPlayerPed();
			auto* pool = CPools::ms_pPedPool;
			if (!player || !pool) {
				return 0;
			}
			const bool  log = Config::Get().debugBody && ::GetTickCount64() >= nextScanLog;
			const auto& cam = TheCamera.m_pFinalCam ? TheCamera.m_pFinalCam->m_mMatrix.pos : player->m_pMatrix->pos;
			int         best = 0;
			float       bestD2 = 1e30f;
			int         sameModel = 0;
			if (log) {
				nextScanLog = ::GetTickCount64() + 1000;
				const auto& pp = player->m_pMatrix->pos;
				LC_LOG("cutscene scan: player ped %d model %d visible %d at %.1f %.1f %.1f; camera %.1f %.1f %.1f; %u peds in the pool", a_player,
					player->m_nModelIndex, S::IS_CHAR_VISIBLE(a_player), pp.x, pp.y, pp.z, cam.x, cam.y, cam.z, static_cast<unsigned>(pool->m_nCount));
			}
			for (int i = 0; i < static_cast<int>(pool->m_nCount); ++i) {
				CPed* p = pool->Get(i);
				if (!p || p == player || !p->m_pMatrix) {
					continue;
				}
				const auto& m = p->m_pMatrix->pos;
				const float dx = m.x - cam.x, dy = m.y - cam.y, dz = m.z - cam.z;
				const float d2 = dx * dx + dy * dy + dz * dz;
				const int   handle = static_cast<int>(pool->GetIndex(p));
				if (log && d2 < 40.0f * 40.0f) {
					B::V3 head;
					const bool hb = ReadBone(handle, kBoneHead, 0, 0, 0, head);
					LC_LOG("  ped %d model %d%s visible %d at %.1f %.1f %.1f (%.1f m from the camera), head bone %s %.2f %.2f %.2f", handle, p->m_nModelIndex,
						p->m_nModelIndex == player->m_nModelIndex ? " (the player's model)" : "", S::IS_CHAR_VISIBLE(handle), m.x, m.y, m.z, std::sqrt(d2),
						hb ? "at" : "missing", head.x, head.y, head.z);
				}
				if (p->m_nModelIndex != player->m_nModelIndex) {
					continue;
				}
				++sameModel;
				if (d2 < bestD2) {
					bestD2 = d2;
					best = handle;
				}
			}
			// (The player ped once the body is on him stays the one: he is invisible because we hid him, and
			// dropping him then showed him again every other frame.)
			const int found = best ? best : (S::IS_CHAR_VISIBLE(a_player) || target == a_player) ? a_player : 0;
			if (log) {
				LC_LOG("cutscene scan: %d other peds of the player's model; following %d", sameModel, found);
				LogCutsceneObjects(player, cam.x, cam.y, cam.z);
			}
			return found;
		}

		std::uint32_t staleFrames = 0;
		bool          lastStale = false;

		bool Read(int a_ped, B::Skeleton& a_s, double (&a_origin)[3])
		{
			if (!a_ped && targetObj) {
				// A cutscene actor's skeleton is only posed when the game draws it (it is transparent
				// under the body): CDynamicEntity::GetBoneMatrix brings a pending pose up to date first.
				reinterpret_cast<CDynamicEntity*>(targetObj)->GetBoneMatrix(0);
			}
			lastStale = false;
			B::V3 pelvis;
			if (!ReadBone(a_ped, kBonePelvis, 0, 0, 0, pelvis)) {
				return false;
			}
			if (!a_ped && targetObj && targetObj->m_pMatrix) {
				const auto& op = targetObj->m_pMatrix->pos;
				if (B::Length(pelvis - B::V3{ op.x, op.y, op.z }) > 2.0f) {
					++staleFrames;  // a pose from another place (not posed since): no body this frame
					lastStale = true;
					return false;
				}
			}
			a_origin[0] = pelvis.x;
			a_origin[1] = pelvis.y;
			a_origin[2] = pelvis.z;
			bool ok = true;
			auto rel = [&](unsigned a_bone, B::V3& a_out, float a_ox = 0.0f, float a_oy = 0.0f, float a_oz = 0.0f) {
				B::V3 w;
				if (!ReadBone(a_ped, a_bone, a_ox, a_oy, a_oz, w) || B::Length(w - pelvis) > kMaxReach) {
					ok = false;
					return;
				}
				a_out = w - pelvis;
			};
			rel(kBoneLThigh, a_s.hipL);
			rel(kBoneRThigh, a_s.hipR);
			rel(kBoneLFoot, a_s.ankleL);
			rel(kBoneRFoot, a_s.ankleR);
			rel(kBoneLCalf, a_s.kneeL);
			rel(kBoneRCalf, a_s.kneeR);
			a_s.knees = true;
			rel(kBoneLUpperArm, a_s.shoulderL);
			rel(kBoneRUpperArm, a_s.shoulderR);
			rel(kBoneLHand, a_s.handL);
			rel(kBoneRHand, a_s.handR);
			rel(kBoneNeck, a_s.neck);
			B::V3 head, ax[3];
			rel(kBoneHead, head);
			a_s.head = head;
			a_s.headPos = true;
			rel(kBoneHead, ax[0], kHeadProbe, 0.0f, 0.0f);
			rel(kBoneHead, ax[1], 0.0f, kHeadProbe, 0.0f);
			rel(kBoneHead, ax[2], 0.0f, 0.0f, kHeadProbe);
			if (!ok || B::Length(a_s.hipL - a_s.hipR) < 0.02f || B::Length(a_s.neck - (a_s.hipL + a_s.hipR) * 0.5f) < 0.1f) {
				return false;
			}
			// The head bone's axes: the offsets follow the bone (a rotation), or come back in world axes.
			bool unit = true, world = true;
			for (int k = 0; k < 3; ++k) {
				a_s.headAxes[k] = (ax[k] - head) * (1.0f / kHeadProbe);
				unit = unit && std::fabs(B::Length(a_s.headAxes[k]) - 1.0f) < 0.05f;
				const float e[3] = { a_s.headAxes[k].x, a_s.headAxes[k].y, a_s.headAxes[k].z };
				world = world && std::fabs(e[k] - 1.0f) < 1e-3f;
			}
			unit = unit && std::fabs(B::Dot(a_s.headAxes[0], a_s.headAxes[1])) < 0.05f && std::fabs(B::Dot(a_s.headAxes[1], a_s.headAxes[2])) < 0.05f &&
			       std::fabs(B::Dot(a_s.headAxes[0], a_s.headAxes[2])) < 0.05f;
			if (world && !worldOffsets) {
				worldOffsets = true;
				LC_LOG("GET_PED_BONE_POSITION's offsets come back in world axes: the head turns with the body");
			}
			a_s.headOk = unit && !world;
			return true;
		}

		void Calibrate(const B::Skeleton& a_s)
		{
			// Only seated in a vehicle (or getting in): the head looks the way the body faces. Tumbling
			// (a knockdown) or in a cutscene it can stay turned for a second and fool the check.
			if (headChecked || !a_s.headOk || targetWhy != drive::Why::kVehicle || !target) {
				return;
			}
			const B::M3 torso = B::Frame(a_s.shoulderL - a_s.shoulderR, a_s.neck - (a_s.hipL + a_s.hipR) * 0.5f);
			B::HeadMap  m;
			if (!B::CalibrateHead(a_s.headAxes, torso, m)) {
				return;
			}
			candidateFrames = m == candidate ? candidateFrames + 1 : 1;
			candidate = m;
			if (candidateFrames >= kCalibrateFrames) {
				headChecked = true;
				static const char* kAxis[] = { "-z", "-y", "-x", "?", "+x", "+y", "+z" };
				if (m == headMap) {
					LC_LOG("head bone check: its axes are the expected ones (left = bone %s, up = %s, forward = %s)", kAxis[m.axis[0] + 3], kAxis[m.axis[1] + 3],
						kAxis[m.axis[2] + 3]);
				} else {
					LC_LOG("WARNING: this skeleton's head bone has other axes: left = bone %s, up = %s, forward = %s (%d frames agreed); using them",
						kAxis[m.axis[0] + 3], kAxis[m.axis[1] + 3], kAxis[m.axis[2] + 3], candidateFrames);
					headMap = m;
				}
			}
		}

		void LogFirst(int a_ped, const B::Skeleton& a_s, const B::Pose& a_pose)
		{
			(a_ped ? loggedFirst : loggedFirstObj) = true;
			auto v = [](const B::V3& a) {
				static char buf[8][48];
				static int  n = 0;
				char*       b = buf[n++ & 7];
				std::snprintf(b, 48, "%.2f %.2f %.2f", a.x, a.y, a.z);
				return b;
			};
			LC_LOG("first pose on %s %d (bones relative to the pelvis): thighs L %s R %s, ankles L %s R %s", a_ped ? "ped" : "cutscene object",
				a_ped ? a_ped : targetObjHandle, v(a_s.hipL), v(a_s.hipR), v(a_s.ankleL),
				v(a_s.ankleR));
			LC_LOG("  shoulders L %s R %s, hands L %s R %s", v(a_s.shoulderL), v(a_s.shoulderR), v(a_s.handL), v(a_s.handR));
			const auto& t = a_pose.torso;
			LC_LOG("  neck %s, knees L %s R %s; body frame left %s up %s forward %s; lift %.2f m (%.2f m of it sunk under the sole match), every part x %.2f",
				v(a_s.neck), v(a_s.kneeL), v(a_s.kneeR), v(t.c[0]), v(t.c[1]), v(t.c[2]), a_pose.lift, a_pose.sink, a_pose.scale);
			LC_LOG("  head bone %s above the pelvis", v(a_s.head));
			LC_LOG("  head bone axes x %s y %s z %s (%s)", v(a_s.headAxes[0]), v(a_s.headAxes[1]), v(a_s.headAxes[2]),
				a_s.headOk ? "a rotation" : "not usable");
			// GTA's "left" bones on the ped's left: the ped's right row (CMatrix right = x).
			CEntity* p = a_ped ? static_cast<CEntity*>(CPools::ms_pPedPool ? CPools::ms_pPedPool->GetAt(static_cast<std::uint32_t>(a_ped)) : nullptr)
			                   : static_cast<CEntity*>(targetObj);
			if (p && p->m_pMatrix) {
				const auto&  r = p->m_pMatrix->right;
				const B::V3  right{ r.x, r.y, r.z };
				LC_LOG("  left thigh on the ped's %s (dot with its right %.2f), body forward vs the ped's forward %.2f", B::Dot(a_s.hipL - a_s.hipR, right) < 0 ? "left" : "RIGHT",
					B::Dot(B::Normalize(a_s.hipL - a_s.hipR, {}), right),
					B::Dot(t.c[2], B::V3{ p->m_pMatrix->up.x, p->m_pMatrix->up.y, p->m_pMatrix->up.z }));
			}
		}

		void Debug(int a_ped, const B::Skeleton& a_s, const B::Pose& a_pose, const double (&a_origin)[3])
		{
			// How much the limbs move relative to the hips (the animation runs while the ped is hidden?).
			const B::V3 hip = (a_s.hipL + a_s.hipR) * 0.5f;
			const B::V3 limbs[4] = { a_s.handL - hip, a_s.handR - hip, a_s.ankleL - hip, a_s.ankleR - hip };
			if (haveLimbs) {
				for (int k = 0; k < 4; ++k) {
					limbMotion += B::Length(limbs[k] - lastLimbs[k]);
				}
			}
			std::memcpy(lastLimbs, limbs, sizeof(limbs));
			haveLimbs = true;
			const auto now = ::GetTickCount64();
			if (!Config::Get().debugBody || now < nextDebug) {
				return;
			}
			nextDebug = now + 1000;
			const auto& t = a_pose.torso;
			const B::V3 h = a_s.handR - hip;
			char        ground[64] = "";
			if (a_ped && !S::IS_CHAR_IN_ANY_CAR(a_ped)) {
				float gl = 0.0f, gr = 0.0f;
				S::GET_GROUND_Z_FOR_3D_COORD(float(a_origin[0] + a_s.ankleL.x), float(a_origin[1] + a_s.ankleL.y), float(a_origin[2] + a_s.ankleL.z + 0.5), &gl);
				S::GET_GROUND_Z_FOR_3D_COORD(float(a_origin[0] + a_s.ankleR.x), float(a_origin[1] + a_s.ankleR.y), float(a_origin[2] + a_s.ankleR.z + 0.5), &gr);
				std::snprintf(ground, sizeof(ground), "; ankles above the ground L %.3f R %.3f", float(a_origin[2] + a_s.ankleL.z) - gl,
					float(a_origin[2] + a_s.ankleR.z) - gr);
			}
			if (!a_ped && targetObj && targetObj->m_pMatrix) {
				const auto& op = targetObj->m_pMatrix->pos;
				const auto& cp = TheCamera.m_pFinalCam ? TheCamera.m_pFinalCam->m_mMatrix.pos : op;
				int         copies = 0;
				if (auto* pool = CPools::ms_pObjectPool) {
					for (int i = pool->FindNextUsed(0); i >= 0; i = pool->FindNextUsed(i + 1)) {
						CObject* o = pool->Get(i);
						copies += o && o->m_nModelIndex == targetObj->m_nModelIndex ? 1 : 0;
					}
				}
				if (CPed* pp = FindPlayerPed(); pp && pp->m_pMatrix) {
					// Other Nikos: peds of the player's model near the cutscene's, and its own scale now.
					int  peds = 0;
					auto* pool = CPools::ms_pPedPool;
					for (int i = 0; pool && i < static_cast<int>(pool->m_nCount); ++i) {
						CPed* q = pool->Get(i);
						if (q && q != pp && q->m_pMatrix && q->m_nModelIndex == pp->m_nModelIndex) {
							const auto& qp = q->m_pMatrix->pos;
							peds += (qp.x - op.x) * (qp.x - op.x) + (qp.y - op.y) * (qp.y - op.y) < 40.0f * 40.0f ? 1 : 0;
						}
					}
					const auto& r = targetObj->m_pMatrix->right;
					LC_LOG("DebugBody: the cutscene's Niko is at scale %.3f now (%.2f wanted); %d other peds of the player's model within 40 m", std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z),
						kGhostScale, peds);
					int handle = 0;
					S::GET_PLAYER_CHAR(static_cast<int>(S::GET_PLAYER_ID()), &handle);
					LC_LOG("DebugBody: the player ped at %.2f %.2f %.2f (%.1f m from the cutscene's Niko), visible %d", pp->m_pMatrix->pos.x, pp->m_pMatrix->pos.y,
						pp->m_pMatrix->pos.z, std::sqrt((pp->m_pMatrix->pos.x - op.x) * (pp->m_pMatrix->pos.x - op.x) + (pp->m_pMatrix->pos.y - op.y) * (pp->m_pMatrix->pos.y - op.y)),
						handle ? S::IS_CHAR_VISIBLE(handle) : -1);
				}
				LC_LOG("DebugBody: cutscene object at %.2f %.2f %.2f, its pelvis bone at %.2f %.2f %.2f, camera %.2f %.2f %.2f; flags %08X alpha %u; %d objects of its model",
					op.x, op.y, op.z, a_origin[0], a_origin[1], a_origin[2], cp.x, cp.y, cp.z, *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(targetObj) + 0x24),
					targetObj->m_nAlpha, copies);
			}
			LC_LOG("DebugBody: %s %d (%s) visible %d, size x %.2f (seated %.2f), lift %.2f (sunk %.2f), head %s, right hand (left/up/forward of the hips) %.2f %.2f %.2f, limbs moved %.2f m in 1 s%s",
				a_ped ? "ped" : "cutscene object", a_ped ? a_ped : targetObjHandle, WhyName(targetWhy), a_ped ? S::IS_CHAR_VISIBLE(a_ped) : targetObjShown ? 1 : 0, a_pose.scale,
				seatedSmooth, a_pose.lift, a_pose.sink, a_pose.headFromBone ? "from its bone" : "with the body", B::Dot(h, t.c[0]), B::Dot(h, t.c[1]),
				B::Dot(h, t.c[2]), limbMotion, ground);
			limbMotion = 0.0;
			// Seated: where the bones are in the vehicle (its right / forward / up axes), against the
			// ped's position this frame and last frame (bones a frame behind a moving car trail it).
			int veh = 0;
			if (a_ped && S::IS_CHAR_IN_ANY_CAR(a_ped)) {
				S::GET_CAR_CHAR_IS_USING(a_ped, &veh);
			}
			CVehicle* v = veh && CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->GetAt(static_cast<std::uint32_t>(veh)) : nullptr;
			CPed*     p = CPools::ms_pPedPool ? CPools::ms_pPedPool->GetAt(static_cast<std::uint32_t>(a_ped)) : nullptr;
			if (v && v->m_pMatrix && p && p->m_pMatrix) {
				const auto& vm = *v->m_pMatrix;
				const B::V3 ax[3] = { { vm.right.x, vm.right.y, vm.right.z }, { vm.up.x, vm.up.y, vm.up.z }, { vm.at.x, vm.at.y, vm.at.z } };
				const B::V3 pelvis{ float(a_origin[0]), float(a_origin[1]), float(a_origin[2]) };
				const B::V3 pedNow{ p->m_pMatrix->pos.x, p->m_pMatrix->pos.y, p->m_pMatrix->pos.z };
				const B::V3 car{ vm.pos.x, vm.pos.y, vm.pos.z };
				auto in = [&](B::V3 a) { return B::V3{ B::Dot(a, ax[0]), B::Dot(a, ax[1]), B::Dot(a, ax[2]) }; };
				const B::V3 a = in(pelvis - pedNow), b = in(pelvis - prevPedPos), c = in(pedNow - car), d = in(pelvis - car);
				float speed = 0.0f;
				S::GET_CAR_SPEED(veh, &speed);
				LC_LOG("DebugBody: in vehicle %d at %.1f m/s: the pelvis bone from the ped's position (right forward up) %.2f %.2f %.2f, from last frame's %.2f %.2f %.2f; "
					   "the ped from the vehicle's origin %.2f %.2f %.2f, the pelvis %.2f %.2f %.2f",
					veh, speed, a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z, d.x, d.y, d.z);
			}
		}

		void Stats()
		{
			const auto now = ::GetTickCount64();
			if (now < nextStats) {
				return;
			}
			const bool any = frames || failed || staleFrames;
			nextStats = now + 10000;
			if (any) {
				LC_LOG("stats 10s: Minecraft body posed in %u frames (%s, %s %d; %u failed reads, %u stale cutscene poses), lift %.2f m on average (sunk %.2f m under the "
					   "sole match), head from its bone in %u; held items hidden (seated, phone) in %u, parts hidden around the camera in %u",
					frames, WhyName(targetWhy), target ? "ped" : "cutscene object", target ? target : targetObjHandle, failed, staleFrames, frames ? liftSum / frames : 0.0,
					frames ? sinkSum / frames : 0.0, headFrames, heldHiddenFrames, cameraInsideFrames);
			}
			frames = failed = headFrames = staleFrames = heldHiddenFrames = cameraInsideFrames = 0;
			liftSum = sinkSum = 0.0;
		}
	}

	namespace
	{
		// DebugTrainRide (test hook): the nearest train carriage, and the player put into it.
		void DebugTrainTick(int a_player)
		{
			const float after = Config::Get().debugTrainRide;
			static std::uint64_t start = 0, nextLog = 0;
			static int           phase = 0, train = 0, missionTrain = 0;
			static bool          made = false;
			auto*                pool = CPools::ms_pVehiclePool;
			CPed*                ped = FindPlayerPed();
			if (after <= 0.0f || !a_player || !pool || !ped || !ped->m_pMatrix) {
				return;
			}
			const auto now = ::GetTickCount64();
			if (!start) {
				start = now;
			}
			if (now < nextLog) {
				return;
			}
			nextLog = now + 2000;
			const auto& pp = ped->m_pMatrix->pos;
			int         nearest = 0, count = 0;
			float       best = 1e30f;
			for (int i = pool->FindNextUsed(0); i >= 0; i = pool->FindNextUsed(i + 1)) {
				CVehicle* v = pool->Get(i);
				if (!v || !v->m_pMatrix || v->m_nVehicleType != VEHICLE_TYPE_TRAIN) {
					continue;
				}
				++count;
				const auto& m = v->m_pMatrix->pos;
				const float d = std::sqrt((m.x - pp.x) * (m.x - pp.x) + (m.y - pp.y) * (m.y - pp.y) + (m.z - pp.z) * (m.z - pp.z));
				if (d < best) {
					best = d;
					nearest = static_cast<int>(pool->GetIndex(v));
				}
			}
			float speed = 0.0f;
			if (phase == 0) {
				if (nearest) {
					S::GET_CAR_SPEED(nearest, &speed);
				}
				LC_LOG("DebugTrainRide: %d train carriages, the nearest %d %.0f m away at %.1f m/s", count, nearest, nearest ? best : 0.0f, speed);
				// None near by then: a mission train on the track point DebugTrainSpot names.
				static bool requested = false;
				float       tx = 0.0f, ty = 0.0f, tz = 0.0f;
				int         cfg = 0;
				const std::string& spot = Config::Get().debugTrainSpot;
				if (!made && (!nearest || best > 200.0f) && double(now - start) >= after * 1000.0 && !spot.empty() &&
					std::sscanf(spot.c_str(), "%f,%f,%f,%d", &tx, &ty, &tz, &cfg) >= 3) {
					const unsigned lo = S::GET_HASH_KEY("subway_lo"), hi = S::GET_HASH_KEY("subway_hi");
					if (!requested) {
						requested = true;
						CStreaming::ScriptRequestModel(static_cast<std::int32_t>(lo));
						CStreaming::ScriptRequestModel(static_cast<std::int32_t>(hi));
						S::SWITCH_RANDOM_TRAINS(true);
						LC_LOG("DebugTrainRide: no train near; subway models requested, random trains on");
					}
					if (S::HAS_MODEL_LOADED(lo) && S::HAS_MODEL_LOADED(hi)) {
						made = true;
						int t = 0;
						S::CREATE_MISSION_TRAIN(static_cast<unsigned>(cfg), tx, ty, tz, true, &t);
						missionTrain = t;
						LC_LOG("DebugTrainRide: CREATE_MISSION_TRAIN(%d) at %.1f %.1f %.1f: train %d", cfg, tx, ty, tz, t);
					}
					return;
				}
				if (nearest && best < 250.0f && double(now - start) >= after * 1000.0) {
					S::WARP_CHAR_INTO_CAR_AS_PASSENGER(a_player, nearest, 0);
					train = nearest;
					phase = 1;
					LC_LOG("DebugTrainRide: the player put into train carriage %d as a passenger", nearest);
				}
				return;
			}
			if (missionTrain && S::DOES_VEHICLE_EXIST(missionTrain)) {
				// A mission train stands still unless told: off at 12 m/s, a subway's cruise.
				S::SET_TRAIN_CRUISE_SPEED(missionTrain, 12.0f);
				if (phase == 1) {
					S::SET_TRAIN_SPEED(missionTrain, 12.0f);
					phase = 2;
				}
			}
			if (train && S::DOES_VEHICLE_EXIST(train)) {
				S::GET_CAR_SPEED(train, &speed);
			}
			LC_LOG("DebugTrainRide: in any car %d, in any train %d, the carriage at %.1f m/s, the player at %.1f %.1f %.1f, visible %d", S::IS_CHAR_IN_ANY_CAR(a_player),
				S::IS_CHAR_IN_ANY_TRAIN(a_player), speed, pp.x, pp.y, pp.z, S::IS_CHAR_VISIBLE(a_player));
		}
	}

	int Target(int a_player, drive::Why a_why, bool a_hostDrives, bool a_mcInWorld)
	{
		DebugTrainTick(a_player);
		const auto& c = Config::Get();
		bool        abOff = false;  // DebugBodyAB: the B half (no body)
		if (c.debugBodyAB > 0.0f) {
			static bool lastAbOff = false;
			abOff = (::GetTickCount64() / static_cast<std::uint64_t>(c.debugBodyAB * 1000.0f)) % 2 == 1;
			if (abOff != lastAbOff) {
				lastAbOff = abOff;
				LC_LOG("DebugBodyAB: %s", abOff ? "B, no Minecraft body" : "A, the Minecraft body");
			}
		}
		const bool  wanted = c.minecraftBody && !abOff && a_hostDrives && a_mcInWorld && a_player && render::World::HasBody() &&
		                    ((a_why == drive::Why::kVehicle && c.minecraftBodyVehicles) ||
		                        ((a_why == drive::Why::kCutscene || a_why == drive::Why::kScript) && c.minecraftBodyCutscenes) ||
		                        (a_why == drive::Why::kNikoMode && c.minecraftBodyNikoMode) || a_why == drive::Why::kRagdoll);
		int ped = 0;
		// Cutscenes: GTA IV's own cutscene actor (an object) first, else a ped (scripted scenes).
		CObject* obj = nullptr;
		int      objHandle = 0;
		if (wanted && a_why == drive::Why::kCutscene) {
			obj = CutsceneActor(FindPlayerPed(), objHandle);
		}
		if (obj != targetObj || objHandle != targetObjHandle) {
			if (hiddenObj && hiddenObj != objHandle) {
				// (Only back to its size while the cutscene goes on: at its end, the screen fading out, the
				// full-size Niko showed for a frame or two before the cutscene removed him.)
				if (CCutsceneMgr::IsRunning() && S::IS_SCREEN_FADED_IN()) {
					ShowObject(hiddenObj);
				}
				hiddenObj = 0;
			}
			if (obj) {
				LC_LOG("Minecraft body on the cutscene's Niko (object %d, model %d)", objHandle, obj->m_nModelIndex);
			} else if (targetObj) {
				LC_LOG("the cutscene's Niko is gone");
			}
			targetObj = obj;
			targetObjHandle = objHandle;
			cachedFrame = 0xFFFFFFFF;
			haveLimbs = false;
		}
		if (wanted && a_why == drive::Why::kCutscene && Config::Get().debugBody && ::GetTickCount64() >= nextScanLog && FindPlayerPed()) {
			nextScanLog = ::GetTickCount64() + 1000;
			const auto& cam = TheCamera.m_pFinalCam ? TheCamera.m_pFinalCam->m_mMatrix.pos : FindPlayerPed()->m_pMatrix->pos;
			LogCutsceneObjects(FindPlayerPed(), cam.x, cam.y, cam.z);
		}
		if (obj) {
			// The cutscene decides whether Niko is in the shot; he is transparent under the body.
			// The cutscene decides whether Niko is in the shot; under the body he shrinks to a doll.
			targetObjShown = (*reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(obj) + 0x24) & (1u << 5)) != 0;
			ScaleObject(obj, kGhostScale);
			hiddenObj = objHandle;
		} else if (wanted) {
			ped = a_why == drive::Why::kCutscene ? CutscenePed(a_player) : a_player;
		}
		const int id = ped ? ped : obj ? -objHandle : 0;  // (objects: negative)
		if (id != lastTarget || (id && a_why != lastWhy)) {
			if (ped) {
				LC_LOG("Minecraft body on %s %d (%s): Niko hidden", ped == a_player ? "the player ped" : "cutscene ped", ped, WhyName(a_why));
			} else if (obj) {
				// (logged above)
			} else if (lastTarget != 0 && lastTarget != -1) {
				LC_LOG("Minecraft body off (%s)", !a_hostDrives ? "Minecraft drives again" : !wanted ? "not wanted here" : "no Niko to follow");
			} else if (wanted && a_why == drive::Why::kCutscene) {
				LC_LOG("cutscene: no Niko to follow (no ped of the player's model, the player ped hidden)");
			}
			lastTarget = id;
			lastWhy = a_why;
		}
		if (ped != target) {
			cachedFrame = 0xFFFFFFFF;
			haveLimbs = false;
		}
		target = ped;
		targetWhy = a_why;
		Stats();
		return ped;
	}

	void Hide(int a_ped, bool a_hide)
	{
		if (!a_ped || !S::DOES_CHAR_EXIST(a_ped)) {
			return;
		}
		if (Config::Get().minecraftBodyHide == "alpha") {
			S::SET_PED_ALPHA(a_ped, a_hide ? 0 : 255);
		} else {
			S::SET_CHAR_VISIBLE(a_ped, !a_hide);
		}
	}

	bool Capture(render::FrameSnapshot& a_f)
	{
		if (!target && !(targetObj && targetObjShown)) {
			return false;
		}
		if (a_f.gameFrame == cachedFrame) {
			if (!cachedOk) {
				return false;
			}
			std::memcpy(a_f.bodyOrigin, cachedOrigin, sizeof(cachedOrigin));
			std::memcpy(a_f.bodyParts, cachedParts, sizeof(cachedParts));
			a_f.flags |= render::kFrameBody | (cachedHideBack ? render::kFrameBodyHideBack : 0u);
			return true;
		}
		cachedFrame = a_f.gameFrame;
		cachedOk = false;
		B::Skeleton s;
		double      origin[3];
		if ((target && !S::DOES_CHAR_EXIST(target)) || !Read(target, s, origin)) {
			if (lastStale) {
				return false;
			}
			++failed;
			LC_LOG_EVERY(5000, "WARNING: couldn't read %s %d's bones: no Minecraft body this frame", target ? "ped" : "cutscene object", target ? target : targetObjHandle);
			return false;
		}
		Calibrate(s);
		// Seated (a car, a chair) the figure is smaller (B::kSeatFit), eased in and out as he sits
		// down and gets up (not tumbling: a knockdown's legs fly anywhere).
		{
			const auto  now = ::GetTickCount64();
			const float dt = std::clamp(float(now - seatedAt) / 1000.0f, 0.0f, 0.25f);
			seatedAt = now;
			const float want = targetWhy == drive::Why::kRagdoll ? 0.0f : B::SeatedAmount(s);
			seatedSmooth += (want - seatedSmooth) * std::min(1.0f, dt * 5.0f);
		}
		B::Pose pose = B::Solve(s, headMap, B::ScaleFor(seatedSmooth) * Config::Get().minecraftBodyScale, seatedSmooth);
		// A cape or an elytra hangs past the hips: seated it came out under the car's floor.
		pose.hideBack = seatedSmooth > 0.3f || (target && S::IS_CHAR_IN_ANY_CAR(target));
		if (target ? !loggedFirst : !loggedFirstObj) {
			LogFirst(target, s, pose);
		}
		// Seated in a vehicle the held items (a sword, a shield) poked through its roof and doors: away
		// while he is in it.
		// GTA's phone out (a call, the phone book): his hand holds the phone, not the pickaxe at his ear.
		const bool seated = target && S::IS_CHAR_IN_ANY_CAR(target);
		const bool phone = target && Input::PhoneOut();
		if (seated || phone) {
			B::HideHeld(pose);
			++heldHiddenFrames;
		}
		if (seated != lastSeated) {
			lastSeated = seated;
			LC_LOG("held items %s", seated ? "hidden: the body sits in a vehicle" : "shown again (out of the vehicle)");
		}
		if (phone != lastPhone) {
			lastPhone = phone;
			LC_LOG("held items %s", phone ? "hidden: GTA's phone is out" : "shown again (the phone is away)");
		}
		// GTA's camera inside the body (a helicopter coming down pushed it in; the Minecraft body is
		// bulkier than Niko): GTA's own camera (knocked over, vehicles) within 0.3 m of the body past its
		// near plane hides all of it, as GTA fades its player; a cutscene's camera (close-ups) only the
		// parts it is in, or so close that its near plane cuts them.
		std::uint32_t inside = 0;
		if (a_f.flags & render::kFrameCameraValid) {
			const B::V3 cam{ float(a_f.camPos[0] - origin[0]), float(a_f.camPos[1] - origin[1]), float(a_f.camPos[2] - origin[2]) };
			// Seated (a car, a train's cinematic cameras inside the carriage) only the parts the camera is
			// at go too: hiding all of it made the body vanish on a train ride whenever the camera came close.
			// (A mission scene counts as a cutscene only while a script's camera shows it; with GTA's own
			// camera behind him the parts near it go with the vehicles' wider margin.)
			const bool  scene = targetWhy == drive::Why::kCutscene || (targetWhy == drive::Why::kScript && Missions::Current().scene);
			inside = B::HideNearCamera(pose, cam, std::clamp(a_f.nearZ, 0.05f, 0.5f) + (scene ? 0.1f : 0.3f), targetWhy == drive::Why::kRagdoll);
		}
		if (inside) {
			++cameraInsideFrames;
		}
		if ((inside != 0) != (lastInside != 0)) {
			LC_LOG("camera %s the Minecraft body%s%s%s%s%s%s", inside ? "at (or in)" : "away from", inside ? ": hidden, near its" : "", (inside & (1u << 1)) ? " head" : "",
				(inside & (1u << 2)) ? " torso" : "", (inside & (3u << 3)) ? " arm" : "", (inside & (3u << 5)) ? " leg" : "", inside ? "" : " again");
		}
		lastInside = inside;
		static_assert(sizeof(cachedParts) == sizeof(pose.part));
		std::memcpy(cachedParts, pose.part, sizeof(cachedParts));
		cachedHideBack = pose.hideBack;
		std::memcpy(cachedOrigin, origin, sizeof(origin));
		cachedOk = true;
		std::memcpy(a_f.bodyOrigin, cachedOrigin, sizeof(cachedOrigin));
		std::memcpy(a_f.bodyParts, cachedParts, sizeof(cachedParts));
		a_f.flags |= render::kFrameBody | (cachedHideBack ? render::kFrameBodyHideBack : 0u);
		++frames;
		headFrames += pose.headFromBone ? 1 : 0;
		liftSum += pose.lift;
		sinkSum += pose.sink;
		if (CPed* p = target && CPools::ms_pPedPool ? CPools::ms_pPedPool->GetAt(static_cast<std::uint32_t>(target)) : nullptr; p && p->m_pMatrix) {
			prevPedPos = lastPedPos;
			lastPedPos = { p->m_pMatrix->pos.x, p->m_pMatrix->pos.y, p->m_pMatrix->pos.z };
		}
		Debug(target, s, pose, origin);
		return true;
	}

	bool DebugCamera(float* a_m)
	{
		const std::string& spec = Config::Get().debugBodyView;
		if (spec.empty() || !a_m) {
			return false;
		}
		struct View
		{
			float angle, dist, height;
		};
		static std::vector<View> views;
		static bool              parsed = false;
		static int               lastView = -1;
		static std::uint64_t     start = 0;
		if (!parsed) {
			parsed = true;
			std::size_t at = 0;
			while (at <= spec.size()) {
				const std::size_t end = std::min(spec.find('|', at), spec.size());
				View v{};
				if (std::sscanf(spec.substr(at, end - at).c_str(), "%f,%f,%f", &v.angle, &v.dist, &v.height) == 3) {
					views.push_back(v);
				}
				at = end + 1;
			}
			LC_LOG("DebugBodyView: %zu views", views.size());
		}
		CPed* player = FindPlayerPed();
		if (views.empty() || !player || !player->m_pMatrix) {
			return false;
		}
		const auto now = ::GetTickCount64();
		if (!start) {
			start = now;
		}
		const int i = static_cast<int>(double(now - start) / (Config::Get().debugBodyViewSeconds * 1000.0)) % static_cast<int>(views.size());
		// Around the body's hips (the pelvis bone), turned with the ped (or the cutscene's Niko).
		CEntity*    ref = targetObj && !target ? static_cast<CEntity*>(targetObj) : static_cast<CEntity*>(player);
		const auto& m = *ref->m_pMatrix;
		B::V3       c{ m.pos.x, m.pos.y, m.pos.z };
		const bool onBody = cachedOk && (target || (targetObj && targetObjShown));
		if (onBody) {
			c = { float(cachedOrigin[0]), float(cachedOrigin[1]), float(cachedOrigin[2]) };
		}
		const B::V3 fwd = B::Normalize(B::V3{ m.up.x, m.up.y, 0.0f }, B::V3{ 0, 1, 0 });
		const B::V3 left{ -fwd.y, fwd.x, 0.0f };
		const View& v = views[static_cast<std::size_t>(i)];
		const float a = v.angle * 3.14159265f / 180.0f;
		const B::V3 pos = c + (fwd * std::cos(a) + left * std::sin(a)) * v.dist + B::V3{ 0, 0, v.height };
		const B::V3 dir = B::Normalize(c + B::V3{ 0, 0, 0.25f } - pos, fwd);
		const B::V3 right = B::Normalize(B::Cross(dir, B::V3{ 0, 0, 1 }), B::V3{ 1, 0, 0 });
		const B::V3 up = B::Cross(right, dir);
		const B::V3 rows[3] = { right, dir, up };
		for (int k = 0; k < 3; ++k) {
			a_m[k * 4 + 0] = rows[k].x;
			a_m[k * 4 + 1] = rows[k].y;
			a_m[k * 4 + 2] = rows[k].z;
		}
		a_m[12] = pos.x;
		a_m[13] = pos.y;
		a_m[14] = pos.z;
		if (i != lastView) {
			lastView = i;
			LC_LOG("DebugBodyView: view %d (%.0f degrees, %.1f m, %.1f m up) at %.2f %.2f %.2f looking at %.2f %.2f %.2f (%s)", i, v.angle, v.dist, v.height, pos.x, pos.y, pos.z,
				c.x, c.y, c.z, onBody ? "the body's hips" : "the player");
		}
		return true;
	}

	void OnIngameStartup()
	{
		target = 0;
		targetObj = nullptr;
		targetObjHandle = hiddenObj = 0;
		lastTarget = -1;
		cachedFrame = 0xFFFFFFFF;
		cachedOk = false;
		haveLimbs = false;
	}
}

// Unity-built into dllmain.cpp (needs IV-SDK). See Render.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "render"
#include "Render.h"

#include "render/Frame.h"
#include "render/Lighting.h"
#include "render/OpaqueDepth.h"
#include "render/RenderMath.h"
#include "render/ShadowPass.h"
#include "render/Shadows.h"
#include "render/World.h"

#include "Config.h"
#include "Coords.h"
#include "Game.h"
#include "Link.h"
#include "Log.h"
#include "NikoBody.h"
#include "Overlay.h"
#include "Perf.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace lc::Render
{
	namespace
	{
		using render::FrameSnapshot;

		// ---- game addresses, found by pattern in GTAIV.exe (the patterns are FusionFix's) -------
		// CRenderPhase::sm_pCurrent: the render phase whose draw list is being built (game thread).
		// It holds its grcViewport at +0xB0 (FusionFix reads its camera height at +0x128).
		std::uintptr_t phaseCurrentAddr = 0;
		// grcViewport::sm_pCurrent.
		std::uintptr_t viewportCurrentAddr = 0;
		bool           scanned = false;
		bool           fusionFix = false;
		constexpr std::uint32_t kPhaseViewportOffset = 0xB0;

		// grcViewport (FusionFix's layout): camera matrix @0x40, view @0x180, projection @0x1C0,
		// width/height @0x2B0, fov @0x2B8, near/far @0x2C0/0x2C4.
		constexpr std::uint32_t kVpCamera = 0x40, kVpView = 0x180, kVpProj = 0x1C0, kVpWidth = 0x2B0, kVpFov = 0x2B8, kVpNear = 0x2C0, kVpBytes = 0x2D0;

		std::uintptr_t Scan(const char* a_pattern)
		{
			std::vector<int> bytes;
			for (const char* p = a_pattern; *p;) {
				if (*p == ' ') {
					++p;
				} else if (*p == '?') {
					bytes.push_back(-1);
					while (*p == '?') {
						++p;
					}
				} else {
					bytes.push_back(static_cast<int>(std::strtoul(p, nullptr, 16)));
					p += 2;
				}
			}
			auto*       base = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			const auto* sec = IMAGE_FIRST_SECTION(nt);
			for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
				if (!(sec->Characteristics & IMAGE_SCN_CNT_CODE)) {
					continue;
				}
				const std::uint8_t* start = base + sec->VirtualAddress;
				const std::size_t   size = sec->Misc.VirtualSize;
				for (std::size_t off = 0; off + bytes.size() <= size; ++off) {
					std::size_t k = 0;
					while (k < bytes.size() && (bytes[k] < 0 || start[off + k] == bytes[k])) {
						++k;
					}
					if (k == bytes.size()) {
						return reinterpret_cast<std::uintptr_t>(start + off);
					}
				}
			}
			return 0;
		}

		std::uintptr_t ScanOperand(std::initializer_list<const char*> a_patterns, std::uint32_t a_offset)
		{
			for (const char* p : a_patterns) {
				if (const auto at = Scan(p)) {
					return *reinterpret_cast<const std::uint32_t*>(at + a_offset);
				}
			}
			return 0;
		}

		void ScanAddresses()
		{
			scanned = true;
			phaseCurrentAddr = ScanOperand({ "89 0D ? ? ? ? 8B 01 FF 50 ? C7 05", "89 0D ? ? ? ? 8B 01 8B 50" }, 2);
			viewportCurrentAddr = ScanOperand({ "8B 35 ? ? ? ? 75 14", "8B 3D ? ? ? ? 75 14 6A 00" }, 2);
			fusionFix = ::GetModuleHandleW(L"GTAIV.EFLC.FusionFix.asi") != nullptr;
			LC_LOG("render: CRenderPhase::sm_pCurrent @%08X, grcViewport::sm_pCurrent @%08X, FusionFix %s", static_cast<unsigned>(phaseCurrentAddr),
				static_cast<unsigned>(viewportCurrentAddr), fusionFix ? "loaded (logarithmic depth)" : "not loaded");
		}

		// Reads game memory that may not be there. Checked with VirtualQuery, not SEH: clang-cl's
		// __try only covers faults in calls (no -fasync-exceptions), so an inlined memcpy's fault
		// isn't caught (tested under Wine: the process dies).
		bool Readable(const void* a_p, std::size_t a_bytes)
		{
			auto       p = reinterpret_cast<std::uintptr_t>(a_p);
			const auto end = p + a_bytes;
			if (p < 0x10000 || end < p) {
				return false;
			}
			while (p < end) {
				MEMORY_BASIC_INFORMATION mbi{};
				if (!::VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
					(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
					!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
					return false;
				}
				p = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
			}
			return true;
		}

		bool SafeCopy(void* a_dst, const void* a_src, std::size_t a_bytes)
		{
			if (!Readable(a_src, a_bytes)) {
				return false;
			}
			std::memcpy(a_dst, a_src, a_bytes);
			return true;
		}

		bool Finite(const float* a_v, int a_n)
		{
			for (int i = 0; i < a_n; ++i) {
				if (!std::isfinite(a_v[i])) {
					return false;
				}
			}
			return true;
		}

		struct ViewportInfo
		{
			render::Mat4 camera, view, proj;
			float        nearZ = 0, farZ = 0, fov = 0;
			std::int32_t width = 0, height = 0;
		};

		// A grcViewport that looks like a perspective 3D camera.
		bool ReadViewport(std::uintptr_t a_vp, ViewportInfo& a_out)
		{
			alignas(16) std::uint8_t buf[kVpBytes];
			if (!a_vp || !SafeCopy(buf, reinterpret_cast<const void*>(a_vp), sizeof(buf))) {
				return false;
			}
			std::memcpy(a_out.camera.m, buf + kVpCamera, 64);
			std::memcpy(a_out.view.m, buf + kVpView, 64);
			std::memcpy(a_out.proj.m, buf + kVpProj, 64);
			std::memcpy(&a_out.width, buf + kVpWidth, 8);
			std::memcpy(&a_out.fov, buf + kVpFov, 4);
			std::memcpy(&a_out.nearZ, buf + kVpNear, 8);
			const auto& p = a_out.proj.m;
			return Finite(&a_out.camera.m[0][0], 16) && Finite(&a_out.view.m[0][0], 16) && Finite(&p[0][0], 16) && std::isfinite(a_out.nearZ) &&
			       std::isfinite(a_out.farZ) && a_out.nearZ > 0.0f && a_out.farZ > a_out.nearZ && std::fabs(std::fabs(p[2][3]) - 1.0f) < 1e-3f &&
			       std::fabs(p[3][3]) < 1e-3f && p[0][0] != 0.0f && p[1][1] != 0.0f;
		}

		bool FromViewport(const ViewportInfo& a_v, FrameSnapshot& a_f)
		{
			const render::Mat4 clip = render::CameraRelativeClip(a_v.view, a_v.proj);
			std::memcpy(a_f.clip, clip.m, sizeof(a_f.clip));
			a_f.camPos[0] = a_v.camera.m[3][0];
			a_f.camPos[1] = a_v.camera.m[3][1];
			a_f.camPos[2] = a_v.camera.m[3][2];
			a_f.nearZ = a_v.nearZ;
			a_f.farZ = a_v.farZ;
			a_f.viewportW = static_cast<std::uint32_t>(std::max(a_v.width, 0));
			a_f.viewportH = static_cast<std::uint32_t>(std::max(a_v.height, 0));
			return true;
		}

		// TheCamera.m_pFinalCam: rows right, forward, up, position (GTA's matrix convention),
		// vertical FOV in degrees.
		bool FromFinalCam(FrameSnapshot& a_f)
		{
			CCam* cam = TheCamera.m_pFinalCam;
			if (!cam) {
				return false;
			}
			float m[16];
			float lens[3];
			if (!SafeCopy(m, &cam->m_mMatrix, sizeof(m)) || !SafeCopy(lens, &cam->m_fFOV, sizeof(lens)) || !Finite(m, 16) || !Finite(lens, 3)) {
				return false;
			}
			const float fov = lens[0] > 1.0f && lens[0] < 170.0f ? lens[0] : 45.0f;
			const float nearZ = lens[1] > 0.0f ? lens[1] : 0.1f;
			const float farZ = lens[2] > nearZ ? lens[2] : 2000.0f;
			const int   w = Game::State().viewportW, h = Game::State().viewportH;
			const float aspect = (w > 0 && h > 0) ? float(w) / float(h) : 16.0f / 9.0f;
			const render::Mat4 clip = render::ClipFromBasis(m + 0, m + 4, m + 8, fov * kDegToRad, aspect, nearZ, farZ);
			std::memcpy(a_f.clip, clip.m, sizeof(a_f.clip));
			a_f.camPos[0] = m[12];
			a_f.camPos[1] = m[13];
			a_f.camPos[2] = m[14];
			a_f.nearZ = nearZ;
			a_f.farZ = farZ;
			return true;
		}

		// ---- the game thread's side ---------------------------------------------------------
		std::uint32_t lastGameFrame = 0xFFFFFFFF;
		std::uint32_t callInFrame = 0;
		DWORD         gameThreadId = 0;
		std::uint64_t nextCameraLog = 0;
		bool          loggedInGameCamera = false;

		const char* SourceName(render::CameraSource a_s)
		{
			switch (a_s) {
			case render::CameraSource::kPhaseViewport:
				return "phase viewport";
			case render::CameraSource::kCurrentViewport:
				return "current viewport";
			case render::CameraSource::kFinalCam:
				return "final cam";
			default:
				return "none";
			}
		}

		// Logs the three camera sources side by side (first frame, then every 10 s with Diagnostics).
		void LogCameras(const ViewportInfo* a_phase, const ViewportInfo* a_current, std::uintptr_t a_phasePtr)
		{
			auto line = [](const char* a_name, const ViewportInfo* a_v) {
				if (!a_v) {
					LC_LOG("camera %s: unavailable", a_name);
					return;
				}
				const auto& c = a_v->camera.m;
				const auto& p = a_v->proj.m;
				LC_LOG("camera %s: pos %.2f %.2f %.2f, rows a (%.2f %.2f %.2f) b (%.2f %.2f %.2f) c (%.2f %.2f %.2f), %dx%d fov %.1f near %.3f far %.1f, "
					   "proj diag %.3f %.3f %.4f [2][3] %.1f [3][2] %.4f",
					a_name, c[3][0], c[3][1], c[3][2], c[0][0], c[0][1], c[0][2], c[1][0], c[1][1], c[1][2], c[2][0], c[2][1], c[2][2], a_v->width, a_v->height,
					a_v->fov, a_v->nearZ, a_v->farZ, p[0][0], p[1][1], p[2][2], p[2][3], p[3][2]);
			};
			std::uint32_t vtbl = 0;
			if (a_phasePtr) {
				SafeCopy(&vtbl, reinterpret_cast<const void*>(a_phasePtr), 4);
			}
			LC_LOG("render phase %08X (vtable %08X)", static_cast<unsigned>(a_phasePtr), vtbl);
			line("phase viewport", a_phase);
			line("current viewport", a_current);
			if (CCam* cam = TheCamera.m_pFinalCam) {
				float m[16];
				if (SafeCopy(m, &cam->m_mMatrix, sizeof(m))) {
					LC_LOG("camera final cam: pos %.2f %.2f %.2f, rows 0 (%.2f %.2f %.2f) 1 (%.2f %.2f %.2f) 2 (%.2f %.2f %.2f), fov %.1f near %.3f far %.1f", m[12],
						m[13], m[14], m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], cam->m_fFOV, cam->m_fNearZ, cam->m_fFarZ);
				}
			}
		}

		// Where does the render phase keep its grcViewport? FusionFix's +0xB0 doesn't hold one in
		// 1.0.8.0, so once in game: look for the final camera's position in the phase object, and
		// one pointer deep, at a grcViewport's camera-matrix position (+0x70), and check that a
		// valid perspective viewport sits around it. Logs what it finds; uses the first hit.
		bool          probedPhase = false;
		std::int32_t  phaseViewportOffset = -1;   // embedded viewport: phase + offset
		std::int32_t  phaseViewportPtrAt = -1;    // pointer to a viewport at phase + this
		bool          PhaseViewport(std::uintptr_t a_phase, ViewportInfo& a_out)
		{
			if (phaseViewportOffset >= 0) {
				return ReadViewport(a_phase + std::uint32_t(phaseViewportOffset), a_out);
			}
			if (phaseViewportPtrAt >= 0) {
				std::uintptr_t vp = 0;
				return SafeCopy(&vp, reinterpret_cast<const void*>(a_phase + std::uint32_t(phaseViewportPtrAt)), 4) && ReadViewport(vp, a_out);
			}
			return ReadViewport(a_phase + kPhaseViewportOffset, a_out);
		}

		void ProbePhase(std::uintptr_t a_phase)
		{
			CCam* cam = TheCamera.m_pFinalCam;
			float m[16];
			if (!cam || !SafeCopy(m, &cam->m_mMatrix, sizeof(m)) || !Finite(m, 16) || (m[12] == 0.0f && m[13] == 0.0f)) {
				return;
			}
			probedPhase = true;
			auto near3 = [&](const float* a_p) { return std::fabs(a_p[0] - m[12]) < 0.05f && std::fabs(a_p[1] - m[13]) < 0.05f && std::fabs(a_p[2] - m[14]) < 0.05f; };
			static std::uint8_t buf[0x1000];
			if (!SafeCopy(buf, reinterpret_cast<const void*>(a_phase), sizeof(buf))) {
				LC_LOG("phase probe: phase %08X unreadable", static_cast<unsigned>(a_phase));
				return;
			}
			int hits = 0;
			for (std::uint32_t off = 0; off + 12 <= sizeof(buf); off += 4) {
				if (near3(reinterpret_cast<const float*>(buf + off))) {
					ViewportInfo v;
					const bool   ok = off >= 0x70 && ReadViewport(a_phase + off - 0x70, v);
					LC_LOG("phase probe: camera position at phase+0x%X%s", off, ok ? " = a grcViewport's camera matrix (viewport at -0x70)" : "");
					if (ok && phaseViewportOffset < 0) {
						phaseViewportOffset = std::int32_t(off - 0x70);
					}
					if (++hits > 8) {
						break;
					}
				}
			}
			for (std::uint32_t off = 0; off + 4 <= 0x400 && phaseViewportOffset < 0; off += 4) {
				std::uintptr_t p = 0;
				std::memcpy(&p, buf + off, 4);
				float pos[3];
				if (p < 0x10000 || (p & 3) || !SafeCopy(pos, reinterpret_cast<const void*>(p + 0x70), sizeof(pos)) || !near3(pos)) {
					continue;
				}
				ViewportInfo v;
				const bool   ok = ReadViewport(p, v);
				LC_LOG("phase probe: phase+0x%X -> %08X looks like a grcViewport (camera position at +0x70)%s", off, static_cast<unsigned>(p), ok ? ", valid" : ", not valid");
				if (ok) {
					phaseViewportPtrAt = std::int32_t(off);
				}
			}
			LC_LOG("phase probe: %s", phaseViewportOffset >= 0 ? "embedded viewport found" : phaseViewportPtrAt >= 0 ? "viewport pointer found" : "no viewport found: final cam it is");
		}

		// ---- test hooks: DebugTimeOfDay / DebugWeather / DebugLightingAB (game thread) --------
		struct DebugStep
		{
			float hour = -1.0f;  // < 0: leave the clock
			int   weather = -1;  // < 0: leave the weather
		};
		std::vector<DebugStep> debugSteps;
		bool                   debugParsed = false;
		int                    debugStepIndex = -1;
		std::uint64_t          debugT0 = 0;
		bool                   debugMinecraftLighting = false;  // DebugLightingAB: first half of each step
		std::atomic<int>       debugLogRequest{ 0 };           // render thread: dump GTA's constants

		std::vector<float> ParseList(const std::string& a_s)
		{
			std::vector<float> out;
			const char* p = a_s.c_str();
			while (*p) {
				char*       end = nullptr;
				const float v = std::strtof(p, &end);
				if (end == p) {
					++p;
					continue;
				}
				out.push_back(v);
				p = end;
			}
			return out;
		}

		// Steps through the hours/weathers (one step every DebugStepSeconds once the blocks are first
		// drawn), pinning GTA's clock each frame. Logs "debug step i/n: ..." for test scripts.
		void DebugSequence(const Config& a_cfg, bool a_drawing)
		{
			if (!debugParsed) {
				debugParsed = true;
				const auto hours = ParseList(a_cfg.debugTimeOfDay), weathers = ParseList(a_cfg.debugWeather);
				const std::size_t n = std::max(hours.size(), weathers.size());
				for (std::size_t i = 0; i < n; ++i) {
					DebugStep s;
					s.hour = hours.empty() ? -1.0f : hours[i % hours.size()];
					s.weather = weathers.empty() ? -1 : int(weathers[i % weathers.size()]);
					debugSteps.push_back(s);
				}
				if (n == 0 && a_cfg.debugLightingAB) {
					debugSteps.push_back(DebugStep{});
				}
			}
			if (debugSteps.empty()) {
				return;
			}
			const auto now = ::GetTickCount64();
			if (!debugT0) {
				if (!a_drawing) {
					return;
				}
				debugT0 = now;
			}
			const float stepS = std::max(2.0f, a_cfg.debugStepSeconds);
			const float t = float(now - debugT0) / 1000.0f;
			const int   index = std::min(int(t / stepS), int(debugSteps.size()) - 1);
			const auto& step = debugSteps[std::size_t(index)];
			const bool  mcLighting = a_cfg.debugLightingAB && std::fmod(t, stepS) < stepS * 0.5f && t < stepS * float(debugSteps.size());
			if (step.hour >= 0.0f) {
				const float h = std::fmod(step.hour, 24.0f);
				CClock::ms_nGameClockHours = std::uint32_t(h);
				CClock::ms_nGameClockMinutes = std::uint32_t((h - std::floor(h)) * 60.0f);
				CClock::ms_nGameClockSeconds = 0;
			}
			if (index != debugStepIndex || mcLighting != debugMinecraftLighting) {
				if (index != debugStepIndex && step.weather >= 0) {
					CWeather::ForceWeatherNow(step.weather);
				}
				debugStepIndex = index;
				debugMinecraftLighting = mcLighting;
				LC_LOG("debug step %d/%zu: time %02u:%02u, weather %u -> %u (%.2f, forced %d, rain %.2f), lighting %s", index + 1, debugSteps.size(),
					CClock::ms_nGameClockHours, CClock::ms_nGameClockMinutes, CWeather::OldWeatherType, CWeather::NewWeatherType, CWeather::InterpolationValue,
					step.weather, CWeather::Rain, (mcLighting || a_cfg.renderLighting != Config::RenderLighting::kGta) ? "minecraft" : "gta");
				debugLogRequest = 2;
			}
		}

		// DebugVehicleSpeed: in a vehicle, push it forward 4 s of every 8 (natives from drawingEvent:
		// a test hook only).
		void DebugVehicleSpeed(const Config& a_cfg)
		{
			if (a_cfg.debugVehicleSpeed <= 0.0f || !Game::State().inVehicle || (::GetTickCount64() / 1000) % 8 >= 4) {
				return;
			}
			namespace S = ::Scripting;
			int ped = 0, veh = 0;
			S::GET_PLAYER_CHAR(static_cast<int>(S::GET_PLAYER_ID()), &ped);
			if (ped && S::IS_CHAR_IN_ANY_CAR(ped)) {
				S::GET_CAR_CHAR_IS_USING(ped, &veh);
				if (veh) {
					S::SET_CAR_FORWARD_SPEED(veh, a_cfg.debugVehicleSpeed);
				}
			}
		}

		// DebugShadowSpot: once, 12 s after the blocks are first drawn, put the player at "x,y,z,heading"
		// (natives from drawingEvent: a test hook only).
		void DebugSpot(const Config& a_cfg, bool a_drawing)
		{
			static bool          done = false;
			static std::uint64_t t0 = 0;
			if (done || a_cfg.debugShadowSpot.empty() || (!t0 && !a_drawing)) {
				return;
			}
			const auto now = ::GetTickCount64();
			if (!t0) {
				t0 = now;
			}
			if (now - t0 < 12000) {
				return;
			}
			done = true;
			const auto v = ParseList(a_cfg.debugShadowSpot);
			if (v.size() < 3) {
				LC_LOG("DebugShadowSpot: want x,y,z[,heading], got \"%s\"", a_cfg.debugShadowSpot.c_str());
				return;
			}
			namespace S = ::Scripting;
			int ped = 0;
			S::GET_PLAYER_CHAR(static_cast<int>(S::GET_PLAYER_ID()), &ped);
			if (!ped || S::IS_CHAR_IN_ANY_CAR(ped)) {
				LC_LOG("DebugShadowSpot: no player on foot");
				return;
			}
			S::SET_CHAR_COORDINATES(ped, v[0], v[1], v[2]);
			if (v.size() > 3) {
				S::SET_CHAR_HEADING(ped, v[3]);
			}
			LC_LOG("DebugShadowSpot: player put at GTA %.1f %.1f %.1f heading %.0f", v[0], v[1], v[2], v.size() > 3 ? v[3] : -1.0f);
		}

		void Capture(FrameSnapshot& a_f, const Config& a_cfg)
		{
			auto& st = Game::State();
			auto& link = Link::Get();
			const bool     alive = link.Valid() && link.MinecraftAlive();
			proto::McState mc{};
			const bool     haveMc = alive && link.ReadMcState(mc);
			const bool     inWorld = haveMc && (mc.flags & proto::kMcInWorld);

			// The camera this frame is rendered with.
			std::uintptr_t phasePtr = 0, currentPtr = 0;
			ViewportInfo   phase, current;
			bool           phaseOk = false, currentOk = false;
			if (phaseCurrentAddr && SafeCopy(&phasePtr, reinterpret_cast<const void*>(phaseCurrentAddr), 4) && phasePtr) {
				if (!probedPhase && !st.gtaMenuOpen && a_f.call == 0) {
					ProbePhase(phasePtr);
				}
				phaseOk = PhaseViewport(phasePtr, phase);
			}
			if (viewportCurrentAddr && SafeCopy(&currentPtr, reinterpret_cast<const void*>(viewportCurrentAddr), 4) && currentPtr) {
				currentOk = ReadViewport(currentPtr, current);
			}
			using RC = Config::RenderCamera;
			bool ok = false;
			if ((a_cfg.renderCamera == RC::kAuto || a_cfg.renderCamera == RC::kPhase) && phaseOk) {
				ok = FromViewport(phase, a_f);
				a_f.source = render::CameraSource::kPhaseViewport;
			} else if (a_cfg.renderCamera == RC::kCurrent && currentOk) {
				ok = FromViewport(current, a_f);
				a_f.source = render::CameraSource::kCurrentViewport;
			} else if (a_cfg.renderCamera == RC::kAuto || a_cfg.renderCamera == RC::kFinalCam) {
				ok = FromFinalCam(a_f);
				a_f.source = render::CameraSource::kFinalCam;
			}
			const auto nowMs = ::GetTickCount64();
			const bool firstInGame = !loggedInGameCamera && !st.gtaMenuOpen && phasePtr;
			if (a_f.call == 0 && (nextCameraLog == 0 || firstInGame || (a_cfg.diagnostics && nowMs >= nextCameraLog))) {
				loggedInGameCamera = loggedInGameCamera || firstInGame;
				nextCameraLog = nowMs + 10000;
				LogCameras(phaseOk ? &phase : nullptr, currentOk ? &current : nullptr, phasePtr);
				LC_LOG("blocks use the %s camera (RenderCamera=%s)", ok ? SourceName(a_f.source) : "NO", a_cfg.renderCamera == RC::kAuto ? "auto" : "pinned");
			}
			if (ok) {
				a_f.flags |= render::kFrameCameraValid;
			}

			// Lighting: GTA's clock (Minecraft's own lighting) and weather; GTA's lighting constants
			// are read back on the render thread.
			a_f.gameHour = float(CClock::ms_nGameClockHours) + float(CClock::ms_nGameClockMinutes) / 60.0f + float(CClock::ms_nGameClockSeconds) / 3600.0f;
			a_f.dayFactor = render::DayFactor(a_f.gameHour);
			a_f.exposure = a_cfg.renderExposure > 0.0f ? a_cfg.renderExposure : 1.0f;
			a_f.rain = CWeather::Rain;
			if (a_cfg.renderLighting == Config::RenderLighting::kGta && !debugMinecraftLighting) {
				a_f.flags |= render::kFrameGtaLighting;
			}
			// Sun shadows; DebugShadowsAB=N: off and on in turn, N s each.
			bool shadows = a_cfg.renderShadows;
			if (a_cfg.debugShadowsAB > 0.0f) {
				static int lastAB = -1;
				const auto period = std::uint64_t(std::max(1.0f, a_cfg.debugShadowsAB) * 1000.0f);
				const bool on = (::GetTickCount64() / period) % 2 == 1;
				shadows = shadows && on;
				if (int(on) != lastAB) {
					lastAB = int(on);
					LC_LOG("DebugShadowsAB: shadows %s", on ? "on" : "off");
				}
			}
			if (shadows) {
				a_f.flags |= render::kFrameShadows;
			}

			using RD = Config::RenderDepth;
			if (a_cfg.renderDepth != RD::kOff) {
				a_f.flags |= render::kFrameDepthTest;
			}
			if (a_cfg.renderDepth == RD::kLog || (a_cfg.renderDepth == RD::kAuto && fusionFix)) {
				a_f.flags |= render::kFrameLogDepth;
			}

			const bool menu = st.gtaMenuOpen;
			const bool puppeting = st.puppeting;
			const bool screenOpen = st.mcScreenOpen;
			// HostDrive: in Niko mode GTA IV is played plainly (its own HUD, no Minecraft HUD); while
			// GTA drives on foot (Niko mode, cutscenes, getting into a car) Niko himself is visible,
			// so Minecraft's body following him would only overlap him. In a vehicle Niko is hidden
			// and the body rides its mount at the seat.
			const bool nikoMode = st.nikoMode;
			const bool hostDrivesOnFoot = st.hostDrives && !st.inVehicle;
			if (alive && !menu && ok) {
				a_f.flags |= render::kFrameDrawWorld;
			}
			using OM = Config::OverlayMode;
			// Minecraft's HUD shows wherever GTA would show its own: while Minecraft drives, and while GTA
			// drives the player in a car or while he gets back up; not in cutscenes, menus or Niko mode.
			const bool hostShowsHud = st.hostDrives && !st.cutscene;
			if (alive && haveMc && !menu && !nikoMode && !st.cutscene && a_cfg.overlay != OM::kOff &&
				(puppeting || screenOpen || hostShowsHud || a_cfg.overlay == OM::kAlways)) {
				a_f.flags |= render::kFrameDrawOverlay;
			}
			if (puppeting) {
				a_f.flags |= render::kFramePuppeting;
			}
			if (screenOpen) {
				a_f.flags |= render::kFrameScreenOpen;
			}
			if (haveMc && puppeting && !screenOpen && mc.cameraMode == 0) {
				a_f.flags |= render::kFrameCrosshair;
			}
			// The player's body: Minecraft sends it relative to its feet whenever it should be seen
			// (third person, riding); 0 batches otherwise.
			if (inWorld && !hostDrivesOnFoot) {
				a_f.flags |= render::kFrameAvatar;
				a_f.feet[0] = mc.x;
				a_f.feet[1] = mc.y;
				a_f.feet[2] = mc.z;
				// In a vehicle Minecraft's rider (and mount) arrive a frame or two behind GTA's seat,
				// which moved on with the vehicle: draw them at the seat of this frame (the ped's
				// matrix, as HostDrive measures it, minus VehicleSeatDrop).
				bool seatFix = true;
				if (a_cfg.debugSeatAB && st.inVehicle) {
					static bool lastFix = true;
					seatFix = ((::GetTickCount64() + 2000) / 4000) % 2 == 0;
					if (seatFix != lastFix) {
						lastFix = seatFix;
						LC_LOG("DebugSeatAB: rider %s", seatFix ? "at GTA's seat (corrected)" : "where Minecraft reports it (uncorrected)");
					}
				}
				CPed* ped = st.inVehicle && seatFix ? FindPlayerPed() : nullptr;
				float pos[3];
				if (ped && ped->m_pMatrix && SafeCopy(pos, &ped->m_pMatrix->pos, sizeof(pos)) && Finite(pos, 3)) {
					const McVec seat = GtaToMc(pos[0], pos[1], pos[2] - a_cfg.vehicleSeatDrop);
					const double dx = seat.x - mc.x, dy = seat.y - mc.y, dz = seat.z - mc.z;
					if (dx * dx + dy * dy + dz * dz < 6.0 * 6.0) {
						a_f.feet[0] = seat.x;
						a_f.feet[1] = seat.y;
						a_f.feet[2] = seat.z;
						a_f.flags |= render::kFrameMountShift;
					}
				}
			}
			// The Minecraft body on Niko's skeleton (MinecraftBody, NikoBody.h) instead of the avatar:
			// while GTA animates Niko (the mount, if shown, still moves to the seat).
			if (inWorld && NikoBody::Capture(a_f)) {
				a_f.flags &= ~render::kFrameAvatar;
			}
			a_f.guiScale = haveMc ? mc.guiScale : 0;
			a_f.cursorX = st.cursorX;
			a_f.cursorY = st.cursorY;
		}

		// ---- our draw command -----------------------------------------------------------------
		// GTA IV's draw commands (CBaseDC): vtable {scalar deleting destructor, Execute, GetSize},
		// +4 a packed id/size word the game fills in (the constructor and Add), then the payload.
		// They're allocated in the draw list being built (CBaseDC::operator new) and run in that
		// order on the render thread. Ours is built by the game's own CDrawRectDC constructor (so
		// the header is exactly what GTA writes), then given our vtable and payload.
		struct RenderDC
		{
			const void*   vtbl;
			std::uint32_t packed;
			FrameSnapshot frame;
		};
		static_assert(offsetof(RenderDC, frame) == 8);
		static_assert(sizeof(RenderDC) < 0x7FF * 16);

		void OnRenderThread(const FrameSnapshot& a_f);

		void* __fastcall DcDestroy(RenderDC* a_this, void*, int)
		{
			return a_this;  // lives in the draw list's memory: nothing to free
		}

		void __fastcall DcExecute(RenderDC* a_this, void*)
		{
			OnRenderThread(a_this->frame);
		}

		int __fastcall DcSize(RenderDC*, void*)
		{
			return static_cast<int>(sizeof(RenderDC));
		}

		const void* const kDcVtable[3] = { reinterpret_cast<const void*>(&DcDestroy), reinterpret_cast<const void*>(&DcExecute),
			reinterpret_cast<const void*>(&DcSize) };

		void Enqueue(const FrameSnapshot& a_f)
		{
			void* mem = CBaseDC::operator new(sizeof(RenderDC), 0);
			if (!mem) {
				return;
			}
			CRect rect{ 0.0f, 0.0f, 0.0f, 0.0f };
			CRGBA color{ 0, 0, 0, 0 };
			::new (mem) CDrawRectDC(&rect, color);
			auto* dc = static_cast<RenderDC*>(mem);
			dc->vtbl = kDcVtable;
			std::memcpy(&dc->frame, &a_f, sizeof(a_f));
			reinterpret_cast<CBaseDC*>(mem)->Add();
		}

		// ---- the render thread's side ----------------------------------------------------------
		DWORD         renderThreadId = 0;
		std::uint32_t drawnFrame = 0xFFFFFFFF;
		std::uint32_t loggedTargets = 0;
		std::uint32_t lastTargetKey = 0;
		std::uint32_t dcCalls = 0, framesDrawn = 0, deviceLostFrames = 0;
		std::uint32_t mountFrames = 0;  // frames the rider was moved to GTA's seat
		double        mountShiftSum = 0.0, mountShiftMax = 0.0;
		std::uint64_t lastStatsMs = 0;
		double        stateMs = 0.0;
		std::uint32_t bbW = 0, bbH = 0, bbCheck = 0;

		double NowMs()
		{
			static const double freq = [] {
				LARGE_INTEGER f;
				::QueryPerformanceFrequency(&f);
				return double(f.QuadPart) / 1000.0;
			}();
			LARGE_INTEGER t;
			::QueryPerformanceCounter(&t);
			return double(t.QuadPart) / freq;
		}

		const char* FormatName(D3DFORMAT a_f, char (&a_buf)[16])
		{
			switch (a_f) {
			case D3DFMT_A8R8G8B8:
				return "A8R8G8B8";
			case D3DFMT_X8R8G8B8:
				return "X8R8G8B8";
			case D3DFMT_A2R10G10B10:
				return "A2R10G10B10";
			case D3DFMT_A16B16G16R16F:
				return "A16B16G16R16F";
			case D3DFMT_D24S8:
				return "D24S8";
			case D3DFMT_D24X8:
				return "D24X8";
			case D3DFMT_D16:
				return "D16";
			case D3DFMT_D32F_LOCKABLE:
				return "D32F";
			case D3DFMT_D24FS8:
				return "D24FS8";
			default:
				break;
			}
			if (static_cast<std::uint32_t>(a_f) > 0xFFFF) {  // FOURCC (INTZ, RAWZ, ...)
				const auto v = static_cast<std::uint32_t>(a_f);
				std::snprintf(a_buf, sizeof(a_buf), "%c%c%c%c", char(v), char(v >> 8), char(v >> 16), char(v >> 24));
			} else {
				std::snprintf(a_buf, sizeof(a_buf), "fmt %u", static_cast<unsigned>(a_f));
			}
			return a_buf;
		}

		// ---- GTA's lighting -----------------------------------------------------------------------
		// GTA IV's shaders share their global parameters at fixed registers (render/Lighting.h). At our
		// draw command (right after GTA's tone mapping) the fog and tone mapping constants and the
		// adapted luminance texture are still bound, but the sun/ambient registers have been reused by
		// later passes. So SetPixelShaderConstantF is watched (a device vtable slot) and the sun pass's
		// values are kept: per frame, the writes while gDirectionalColour holds its brightest sun/moon.
		// GTA's sun shadow constants (render/Shadows.h): the pixel shader globals c53-c63 its sun pass set
		// (the set most writes used this frame, like the sun) and FusionFix's c217-c223 as last written.
		namespace shadowwatch
		{
			using render::shreg::kFirst;
			using render::shreg::kCount;
			using render::shreg::kFfFirst;
			using render::shreg::kFfCount;
			constexpr std::uint32_t kNone = 0xFFFFFFFF;
			struct Seen
			{
				render::GtaShadowSet set;
				std::uint32_t        count;
			};
			render::GtaShadowSet     cur;  // c53-c63 as last written (c55 is no parameter: left at 0)
			constexpr std::uint32_t  kSeen = 32;
			Seen                     seen[kSeen]{};
			std::uint32_t            seenCount = 0, lastIdx = kNone, writes = 0;
			render::FusionShadowTune ff;

			void Watch(UINT a_start, const float* a_data, UINT a_count)
			{
				if (!a_data || !a_count) {
					return;
				}
				const UINT end = a_start + a_count;
				if (end > kFfFirst && a_start < kFfFirst + kFfCount) {
					const UINT from = std::max<UINT>(a_start, kFfFirst), to = std::min<UINT>(end, kFfFirst + kFfCount);
					std::memcpy(ff.r[from - kFfFirst], a_data + (from - a_start) * 4, (to - from) * 16);
					ff.known = true;
				}
				if (end <= kFirst || a_start >= kFirst + kCount) {
					return;
				}
				const UINT from = std::max<UINT>(a_start, kFirst), to = std::min<UINT>(end, kFirst + kCount);
				bool       changed = false;
				for (UINT r = from; r < to; ++r) {
					const float* v = a_data + (r - a_start) * 4;
					if (r != kFirst + 2 && std::memcmp(cur.r[r - kFirst], v, 16) != 0) {
						std::memcpy(cur.r[r - kFirst], v, 16);
						changed = true;
					}
				}
				++writes;
				if (!changed && lastIdx < seenCount) {
					++seen[lastIdx].count;
					return;
				}
				for (std::uint32_t k = 0; k < seenCount; ++k) {
					if (std::memcmp(&seen[k].set, &cur, sizeof(cur)) == 0) {
						++seen[k].count;
						lastIdx = k;
						return;
					}
				}
				// A new set (or one half-way between two: GTA sets each register on its own). A full table
				// makes room by dropping the least used.
				std::uint32_t k = seenCount;
				if (seenCount < kSeen) {
					++seenCount;
				} else {
					k = 0;
					for (std::uint32_t i = 1; i < kSeen; ++i) {
						if (seen[i].count < seen[k].count) {
							k = i;
						}
					}
				}
				seen[k].set = cur;
				seen[k].count = 1;
				lastIdx = k;
			}

			void Reset()
			{
				seenCount = 0;
				writes = 0;
				lastIdx = kNone;
			}

			// This frame's set: the valid one most writes left in the registers among those made for this
			// frame's camera (a_cam, a_fwd: GTA's cascade thresholds, gFacetCentre, are the cascades' ends
			// plus the camera's own depth; other viewports, a water reflection, and the half-way sets of a
			// switch between them don't fit). Then start over.
			bool Take(const double a_cam[3], const float a_fwd[3], render::GtaShadowSet& a_out, std::uint32_t& a_writes, std::uint32_t& a_distinct,
				std::uint32_t& a_otherCamera)
			{
				int best = -1;
				a_otherCamera = 0;
				for (std::uint32_t k = 0; k < seenCount; ++k) {
					if (!render::ShadowSetValid(seen[k].set)) {
						continue;
					}
					if (!render::ShadowSetFitsCamera(seen[k].set, a_cam, a_fwd)) {
						a_otherCamera += seen[k].count;
						continue;
					}
					if (best < 0 || seen[k].count > seen[best].count) {
						best = int(k);
					}
				}
				if (best >= 0) {
					a_out = seen[best].set;
				}
				a_writes = best >= 0 ? seen[best].count : 0;
				a_distinct = seenCount;
				Reset();
				return best >= 0;
			}
		}

		namespace sunwatch
		{
			using SetF = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
			// IDirect3DDevice9 vtable order: SetVertexShaderConstantF 94, SetPixelShaderConstantF 109.
			constexpr unsigned         kSlots[2] = { 109, 94 };
			const char* const          kStageNames[2] = { "pixel", "vertex" };
			SetF                       original[2]{};
			bool                       tried = false;
			std::atomic<std::uint32_t> calls{ 0 };

			struct Seen
			{
				float         regs[4][4];  // c17, c18, c37, c38
				std::uint32_t count, firstCall;
			};
			struct Stage
			{
				float         shadow[48][4]{};  // c0-c47 as last written
				Seen          seen[12]{};       // DebugLighting: the distinct sun/ambient sets this frame
				std::uint32_t seenCount = 0, frameCalls = 0, frameTouch = 0;
			};
			Stage          stages[2];
			bool           recordSeen = false;
			// GTA's tone mapping pass, caught as it sets its constants (c64-c95; a later pass reuses
			// some of them before our draw command runs).
			float          high[32][4]{};
			float          texelX = 0.0f, texelY = 0.0f;  // 1 / the back buffer size (set by the render thread)
			render::GtaTone tone;
			unsigned       toneReg = 0;  // ToneMapParams register of the variant seen this frame (0: none)

			void WatchHigh(UINT a_start, const float* a_data, UINT a_count)
			{
				if (!a_data || a_start + a_count <= 64 || a_start >= 96) {
					return;
				}
				const UINT from = std::max<UINT>(a_start, 64), to = std::min<UINT>(a_start + a_count, 96);
				for (UINT r = from; r < to; ++r) {
					const float* v = a_data + (r - a_start) * 4;
					// c66: the tone mapping (and bloom) passes write Exposure as (x, 0, 0, 0); a later
					// pass reuses c66 for a colour, which must not count as the exposure.
					if (r == render::gtareg::kExposure && (v[1] != 0.0f || v[2] != 0.0f || v[3] != 0.0f)) {
						continue;
					}
					std::memcpy(high[r - 64], v, 16);
				}
				if (texelX > 0.0f && from <= 84 && to > 66) {
					render::GtaTone t;
					if (const unsigned reg = render::FindTone(high, texelX, texelY, t)) {
						tone = t;
						toneReg = reg;
					}
				}
			}

			void Watch(int a_stage, UINT a_start, const float* a_data, UINT a_count)
			{
				auto& st = stages[a_stage];
				++st.frameCalls;
				if (!a_data || a_start >= 48 || !a_count) {
					return;
				}
				const UINT n = std::min<UINT>(a_count, 48 - a_start);
				std::memcpy(st.shadow[a_start], a_data, n * 16);
				auto has = [&](UINT a_r) { return a_r >= a_start && a_r < a_start + n; };
				if (!(has(render::gtareg::kDirLight) || has(render::gtareg::kDirColour) || has(render::gtareg::kAmbient0) || has(render::gtareg::kAmbient1))) {
					return;
				}
				++st.frameTouch;
				float cur[4][4];
				std::memcpy(cur[0], st.shadow[render::gtareg::kDirLight], 16);
				std::memcpy(cur[1], st.shadow[render::gtareg::kDirColour], 16);
				std::memcpy(cur[2], st.shadow[render::gtareg::kAmbient0], 16);
				std::memcpy(cur[3], st.shadow[render::gtareg::kAmbient1], 16);
				// The distinct register sets of this frame and how many draws used each: the scene's
				// sun pass and its geometry use one set hundreds of times; a few other passes (an
				// interior's partial writes, a second light direction for a handful of draws) differ.
				std::uint32_t k = 0;
				while (k < st.seenCount && std::memcmp(st.seen[k].regs, cur, sizeof(cur)) != 0) {
					++k;
				}
				if (k < st.seenCount) {
					++st.seen[k].count;
				} else if (k < 12) {
					std::memcpy(st.seen[k].regs, cur, sizeof(cur));
					st.seen[k].count = 1;
					st.seen[k].firstCall = st.frameCalls;
					st.seenCount = k + 1;
				}
			}

			// Set while our draw command draws (render thread): our own writes aren't GTA's.
			bool ours = false;

			HRESULT STDMETHODCALLTYPE HookPs(IDirect3DDevice9* a_d, UINT a_start, const float* a_data, UINT a_count)
			{
				calls.fetch_add(1, std::memory_order_relaxed);
				if (!ours) {
					Watch(0, a_start, a_data, a_count);
					WatchHigh(a_start, a_data, a_count);
					shadowwatch::Watch(a_start, a_data, a_count);
				}
				return original[0](a_d, a_start, a_data, a_count);
			}

			HRESULT STDMETHODCALLTYPE HookVs(IDirect3DDevice9* a_d, UINT a_start, const float* a_data, UINT a_count)
			{
				if (!ours) {
					Watch(1, a_start, a_data, a_count);
				}
				return original[1](a_d, a_start, a_data, a_count);
			}

			bool Patch(void** a_slot, void* a_value)
			{
				DWORD old = 0;
				if (!::VirtualProtect(a_slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
					return false;
				}
				*a_slot = a_value;
				::VirtualProtect(a_slot, sizeof(void*), old, &old);
				::FlushInstructionCache(::GetCurrentProcess(), a_slot, sizeof(void*));
				return true;
			}

			void Install(IDirect3DDevice9* a_d)
			{
				tried = true;
				auto**     vtbl = *reinterpret_cast<void***>(a_d);
				void*const hooks[2] = { reinterpret_cast<void*>(&HookPs), reinterpret_cast<void*>(&HookVs) };
				for (int s = 0; s < 2; ++s) {
					void** slot = vtbl + kSlots[s];
					void*  was = *slot;
					if (!Patch(slot, hooks[s])) {
						LC_LOG("WARNING: can't watch Set%sShaderConstantF (VirtualProtect error %lu)", s ? "Vertex" : "Pixel", ::GetLastError());
						continue;
					}
					original[s] = reinterpret_cast<SetF>(was);
				}
				if (!original[0]) {
					LC_LOG("WARNING: GTA's sun/ambient unknown: Minecraft's lighting");
					return;
				}
				// Check the slot: our own call must come through.
				const auto  before = calls.load();
				const float probe[4] = { 0, 0, 0, 0 };
				a_d->SetPixelShaderConstantF(47, probe, 1);
				if (calls.load() == before) {
					for (int s = 0; s < 2; ++s) {
						if (original[s]) {
							Patch(vtbl + kSlots[s], reinterpret_cast<void*>(original[s]));
							original[s] = nullptr;
						}
					}
					LC_LOG("WARNING: vtable slot %u isn't SetPixelShaderConstantF: unhooked; GTA's sun/ambient unknown", kSlots[0]);
					return;
				}
				LC_LOG("watching Set{Pixel,Vertex}ShaderConstantF (device %p, vtable %p) for GTA's sun and ambient", static_cast<void*>(a_d), static_cast<void*>(vtbl));
			}

			// Forget this frame's writes (a frame without the blocks: nothing to light).
			void Reset()
			{
				for (auto& st : stages) {
					st.seenCount = st.frameCalls = st.frameTouch = 0;
				}
				toneReg = 0;
			}

			// This frame's tone mapping constants, then start over for the next frame.
			unsigned TakeTone(render::GtaTone& a_out)
			{
				const unsigned reg = toneReg;
				a_out = tone;
				toneReg = 0;
				return reg;
			}

			// This frame's sun pass (the valid pixel shader set most draws used), then start over for
			// the next frame.
			bool Take(render::GtaSun& a_out, std::uint32_t& a_writes, std::uint32_t& a_distinct)
			{
				const auto& ps = stages[0];
				int         best = -1;
				for (std::uint32_t k = 0; k < ps.seenCount; ++k) {
					render::GtaSun c;
					std::memcpy(c.dir, ps.seen[k].regs[0], 16);
					std::memcpy(c.colour, ps.seen[k].regs[1], 16);
					std::memcpy(c.amb0, ps.seen[k].regs[2], 16);
					std::memcpy(c.amb1, ps.seen[k].regs[3], 16);
					if (render::SunValid(c) && (best < 0 || ps.seen[k].count > ps.seen[best].count)) {
						best = int(k);
						a_out = c;
					}
				}
				a_writes = best >= 0 ? ps.seen[best].count : 0;
				a_distinct = ps.seenCount;
				const bool any = original[0] && best >= 0;
				for (int s = 0; s < 2; ++s) {
					auto& st = stages[s];
					if (recordSeen && (s == 0 || Config::Get().diagnostics)) {
						LC_LOG("sun watch, %s shader constants: %u calls this frame, %u touching c17/c18/c37/c38, %u distinct sets:", kStageNames[s], st.frameCalls,
							st.frameTouch, st.seenCount);
						for (std::uint32_t k = 0; k < st.seenCount; ++k) {
							const auto& r = st.seen[k].regs;
							LC_LOG("  x%u from call %u: c17 %.3f %.3f %.3f %.3f c18 %.3f %.3f %.3f %.3f c37 %.3f %.3f %.3f %.3f c38 %.3f %.3f %.3f %.3f", st.seen[k].count,
								st.seen[k].firstCall, r[0][0], r[0][1], r[0][2], r[0][3], r[1][0], r[1][1], r[1][2], r[1][3], r[2][0], r[2][1], r[2][2], r[2][3],
								r[3][0], r[3][1], r[3][2], r[3][3]);
						}
					}
					st.seenCount = st.frameCalls = st.frameTouch = 0;
				}
				recordSeen = false;
				return any;
			}
		}

		render::LightingInputs lastInputs;
		bool                   loggedInputs = false;
		bool                   lastToneWatched = false;
		std::uint64_t          nextLightLog = 0;
		render::GtaSun         heldSun;              // the last good sun pass (a frame without one keeps it)
		bool                   heldSunOk = false;
		std::uint32_t          sunMissingFrames = 0;

		const char* Ok(bool a_ok)
		{
			return a_ok ? "ok" : "missing";
		}

		// GTA's adapted luminance: the 1x1 float texture its tone mapping sampled (s5 in the full
		// variant). Not AddRef'd beyond this frame's use.
		IDirect3DBaseTexture9* FindAdaptedLuminance(IDirect3DDevice9* a_d, DWORD& a_stage)
		{
			static const DWORD kOrder[] = { 5, 1, 2, 3, 4, 0, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
			for (const DWORD s : kOrder) {
				IDirect3DBaseTexture9* t = nullptr;
				if (FAILED(a_d->GetTexture(s, &t)) || !t) {
					continue;
				}
				D3DSURFACE_DESC desc{};
				const bool      ok = t->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &desc)) && desc.Width == 1 &&
				                desc.Height == 1 && (desc.Format == D3DFMT_R32F || desc.Format == D3DFMT_R16F || desc.Format == D3DFMT_G16R16F || desc.Format == D3DFMT_G32R32F);
				t->Release();
				if (ok) {
					a_stage = s;
					return t;  // still bound to stage s, so it stays alive through our draw
				}
			}
			return nullptr;
		}

		// DebugLighting: the registers and textures around, raw.
		void DumpGtaState(IDirect3DDevice9* a_d)
		{
			float r[96][4]{};
			a_d->GetPixelShaderConstantF(0, &r[0][0], 96);
			for (unsigned i = 16; i < 96; i += 2) {
				if (i == 48) {
					i = 64;
				}
				LC_LOG("  c%u %.4g %.4g %.4g %.4g | c%u %.4g %.4g %.4g %.4g", i, r[i][0], r[i][1], r[i][2], r[i][3], i + 1, r[i + 1][0], r[i + 1][1], r[i + 1][2], r[i + 1][3]);
			}
			float ff[12][4]{};
			a_d->GetPixelShaderConstantF(212, &ff[0][0], 12);
			for (unsigned i = 0; i < 12; i += 2) {
				LC_LOG("  c%u %.4g %.4g %.4g %.4g | c%u %.4g %.4g %.4g %.4g", 212 + i, ff[i][0], ff[i][1], ff[i][2], ff[i][3], 213 + i, ff[i + 1][0], ff[i + 1][1],
					ff[i + 1][2], ff[i + 1][3]);
			}
			for (DWORD s = 0; s < 16; ++s) {
				IDirect3DBaseTexture9* t = nullptr;
				if (FAILED(a_d->GetTexture(s, &t)) || !t) {
					continue;
				}
				D3DSURFACE_DESC desc{};
				char            fmt[16];
				if (t->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &desc))) {
					LC_LOG("  sampler %lu: texture %p %ux%u %s", s, static_cast<void*>(t), desc.Width, desc.Height, FormatName(desc.Format, fmt));
				}
				t->Release();
			}
		}

		// Everything GTA's picture was made with this frame -> the world shader's parameters.
		render::LightingParams GtaLighting(IDirect3DDevice9* a_d, const FrameSnapshot& a_f, std::uint32_t a_rtW, std::uint32_t a_rtH,
			IDirect3DBaseTexture9*& a_adapted)
		{
			const auto&            cfg = Config::Get();
			render::LightingInputs in;
			in.rain = a_f.rain;
			a_adapted = nullptr;
			unsigned toneReg = 0;
			bool     toneWatched = false;
			if (!sunwatch::tried) {
				sunwatch::Install(a_d);
			}
			std::uint32_t writes = 0, distinct = 0;
			render::GtaSun sun;
			if (sunwatch::Take(sun, writes, distinct) && render::SunValid(sun)) {
				heldSun = sun;
				heldSunOk = true;
				sunMissingFrames = 0;
			} else if (++sunMissingFrames > 30) {
				heldSunOk = false;  // half a second without a sun pass (a loading screen, a cutscene cut): don't keep stale light
			}
			in.sun = heldSun;
			in.sunOk = heldSunOk;

			float r[96][4]{};
			if (SUCCEEDED(a_d->GetPixelShaderConstantF(0, &r[0][0], 96))) {
				std::memcpy(in.fog.params, r[render::gtareg::kFogParams], 16);
				std::memcpy(in.fog.colour, r[render::gtareg::kFogColor], 16);
				std::memcpy(in.fog.colourN, r[render::gtareg::kFogColorN], 16);
				std::memcpy(in.fog.depthFx, r[render::gtareg::kDepthFx], 16);
				in.fogOk = render::FogValid(in.fog);
				in.depthFxOk = render::DepthFxValid(in.fog);
				// The tone mapping as its pass set it (watched), else what is left in the registers.
				toneReg = sunwatch::TakeTone(in.tone);
				toneWatched = toneReg != 0;
				if (!toneWatched) {
					float high[32][4];
					std::memcpy(high, r[64], sizeof(high));
					toneReg = a_rtW && a_rtH ? render::FindTone(high, 1.0f / float(a_rtW), 1.0f / float(a_rtH), in.tone) : 0;
				}
				in.toneOk = toneReg != 0;
			}
			if (a_rtW && a_rtH) {
				sunwatch::texelX = 1.0f / float(a_rtW);
				sunwatch::texelY = 1.0f / float(a_rtH);
			}
			DWORD stage = 0;
			if (in.toneOk) {
				a_adapted = FindAdaptedLuminance(a_d, stage);
				in.adaptedTexture = a_adapted != nullptr;
			}
			render::ExposureTuning tune;
			tune.exposure = a_f.exposure;
			tune.key = cfg.renderExposureKey;
			tune.floor = cfg.renderExposureFloor;
			tune.saturation = cfg.renderSaturation;
			const auto p = render::MakeLighting(in, tune);

			const bool changed = !loggedInputs || in.sunOk != lastInputs.sunOk || in.fogOk != lastInputs.fogOk || in.toneOk != lastInputs.toneOk ||
			                     in.adaptedTexture != lastInputs.adaptedTexture || toneWatched != lastToneWatched;
			lastToneWatched = toneWatched;
			const auto now = ::GetTickCount64();
			const int  dump = debugLogRequest.exchange(0);
			if (changed || dump || (cfg.debugLighting && now >= nextLightLog)) {
				nextLightLog = now + 10000;
				loggedInputs = true;
				lastInputs = in;
				const auto& s = in.sun;
				const auto& t = in.tone;
				LC_LOG("GTA lighting (hour %.2f, rain %.2f): %s; sun %s (%u draws of %u sets) towards %.2f %.2f %.2f colour %.3f %.3f %.3f x %.2f, ambient %.3f %.3f %.3f "
					   "+ down %.3f %.3f %.3f; fog %s %.1f to %.1f m (+%.2f, near %.2f) far %.3f %.3f %.3f near %.3f %.3f %.3f; depthFx %s %.2f %.2f %.1f to %.1f m; "
					   "tone %s (ToneMapParams c%u, %s) exposure %.3f key %.3f saturation %.2f gamma %.3f correct %.3f %.3f %.3f shift %.2f %.2f %.2f x %.0f; adapted luminance %s",
					a_f.gameHour, a_f.rain, p.sunDir[3] > 0.5f ? "GTA's lighting" : "Minecraft's lighting (no sun pass seen)", Ok(in.sunOk), writes, distinct, -s.dir[0],
					-s.dir[1], -s.dir[2], s.colour[0], s.colour[1], s.colour[2], s.colour[3], s.amb0[0], s.amb0[1], s.amb0[2], s.amb1[0], s.amb1[1], s.amb1[2],
					Ok(in.fogOk), in.fog.params[0], in.fog.params[1], in.fog.params[2], in.fog.params[3], in.fog.colour[0], in.fog.colour[1], in.fog.colour[2],
					in.fog.colourN[0], in.fog.colourN[1], in.fog.colourN[2], Ok(in.depthFxOk), in.fog.depthFx[0], in.fog.depthFx[1], in.fog.depthFx[2],
					in.fog.depthFx[3], Ok(in.toneOk), toneReg, toneWatched ? "as its pass set it" : "left in the registers", t.exposure, t.tmp[1], t.dsg[0], t.dsg[2], t.cc[0], t.cc[1], t.cc[2], t.cs[0], t.cs[1], t.cs[2], t.cs[3],
					in.adaptedTexture ? (stage == 5 ? "GTA's texture (s5)" : "GTA's texture (not s5)") : "stand-in");
				if (dump && cfg.debugLighting) {
					DumpGtaState(a_d);
				}
				sunwatch::recordSeen = cfg.debugLighting;  // the next frame's writes, listed at its Take
			}
			return p;
		}

		// ---- GTA's sun shadows (render/Shadows.h) ------------------------------------------------------
		render::GtaShadowSet heldShadow;
		bool                 heldShadowOk = false;
		std::uint32_t        shadowMissingFrames = 0;
		int                  lastShadowState = -1;
		std::uint64_t        nextShadowLog = 0;
		// Frames with shadows on, by where GTA's set came from: this frame's, or one held from an earlier
		// frame (GTA's sun pass set none for this camera); and how often the chosen set changed.
		std::uint32_t        shadowFreshFrames = 0, shadowHeldFrames = 0, shadowSetChanges = 0;

		// This frame's ShadowFrame: GTA's shadow constants and its cascade atlas (on sampler 15 at our draw
		// command). Returns the atlas (AddRef'd: the caller releases it after drawing) or null: no shadows.
		bool loggedGBuffer = false;

		// GTA's G-buffer 2 (its z scales the ambient in GTA's sun pass): its tone mapping pass, right before
		// our draw command, samples it on s0. A texture the back buffer's size, A8R8G8B8 (FusionFix's
		// G-buffer format). Not AddRef'd beyond this frame (it stays bound to s0 until we change it, and
		// GTA's render target lives on).
		IDirect3DBaseTexture9* GtaGBuffer2(IDirect3DDevice9* a_d, std::uint32_t a_w, std::uint32_t a_h)
		{
			IDirect3DBaseTexture9* t = nullptr;
			D3DSURFACE_DESC        d{};
			const bool ok = SUCCEEDED(a_d->GetTexture(0, &t)) && t && t->GetType() == D3DRTYPE_TEXTURE &&
			                SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d)) && d.Width == a_w && d.Height == a_h &&
			                (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8) && (d.Usage & D3DUSAGE_RENDERTARGET);
			if (!loggedGBuffer && t) {
				loggedGBuffer = true;
				char fmt[16];
				LC_LOG("GTA's G-buffer 2 (ambient occlusion for the blocks' shadow on GTA's world) on s0: %s (%ux%u %s, usage 0x%lX)", ok ? "yes" : "no", d.Width, d.Height,
					FormatName(d.Format, fmt), static_cast<unsigned long>(d.Usage));
			}
			if (t) {
				t->Release();
			}
			return ok ? t : nullptr;
		}

		IDirect3DBaseTexture9* GtaShadows(IDirect3DDevice9* a_d, const FrameSnapshot& a_f, const render::LightingParams& a_light, std::uint32_t a_rtW,
			std::uint32_t a_rtH, render::ShadowFrame& a_out)
		{
			const auto&          cfg = Config::Get();
			const auto           tuning = render::TuningFrom(shadowwatch::ff);
			render::shadowpass::SetNdlRemap(tuning.ndlScale, tuning.ndlOffset);
			render::GtaShadowSet set;
			std::uint32_t        writes = 0, distinct = 0, otherCamera = 0;
			const float          fwd[3] = { a_f.clip[0][3], a_f.clip[1][3], a_f.clip[2][3] };
			const bool fresh = shadowwatch::Take(a_f.camPos, fwd, set, writes, distinct, otherCamera);
			if (fresh) {
				if (std::memcmp(&set, &heldShadow, sizeof(set)) != 0) {
					++shadowSetChanges;
				}
				heldShadow = set;
				heldShadowOk = true;
				shadowMissingFrames = 0;
			} else if (++shadowMissingFrames > 30) {
				heldShadowOk = false;  // half a second without GTA's sun pass using shadows (interiors, loading)
			}
			IDirect3DBaseTexture9* t = nullptr;
			D3DSURFACE_DESC        desc{};
			bool                   atlasOk = false;
			if (SUCCEEDED(a_d->GetTexture(15, &t)) && t) {
				atlasOk = t->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &desc)) &&
				          desc.Format == D3DFMT_R32F && desc.Width == desc.Height * 4 && desc.Height >= 64;
			}
			const bool want = (a_f.flags & render::kFrameShadows) && (a_f.flags & render::kFrameGtaLighting) && a_light.sunDir[3] > 0.5f;
			const bool ok = want && heldShadowOk && atlasOk;
			if (ok) {
				++(fresh ? shadowFreshFrames : shadowHeldFrames);
				a_out.params = render::MakeShadowParams(heldShadow, tuning, a_f.camPos, fwd, true);
				a_out.gbuffer2 = GtaGBuffer2(a_d, a_rtW, a_rtH);
				a_out.gtaAtlas = t;
				a_out.atlasW = desc.Width;
				a_out.atlasH = desc.Height;
				a_out.cast = cfg.renderShadowCast;
				const float* mz = a_out.params.mz;
				a_out.casterBias = cfg.renderShadowBias * std::sqrt(mz[0] * mz[0] + mz[1] * mz[1] + mz[2] * mz[2]);
				a_out.strength = cfg.renderShadowStrength;
				a_out.distance = cfg.renderShadowDistance;
				a_out.view = cfg.debugShadowView;
				// DebugShadows: both atlases to <gamedir>/libertycraft-shadow-*.pgm, 15 s after the shadows came on, then every 30 s.
				static std::uint64_t nextDump = 0;
				const auto           nowMs = ::GetTickCount64();
				if (cfg.debugShadows && (nextDump == 0 || nowMs >= nextDump)) {
					a_out.dump = nextDump != 0;
					nextDump = nowMs + (nextDump == 0 ? 15000 : 30000);
				}
			}
			const int  state = (want ? 1 : 0) | (heldShadowOk ? 2 : 0) | (atlasOk ? 4 : 0);
			const auto now = ::GetTickCount64();
			if (state != lastShadowState || (cfg.debugShadows && now >= nextShadowLog)) {
				nextShadowLog = now + 10000;
				const bool first = lastShadowState < 0;
				lastShadowState = state;
				char fmt[16];
				LC_LOG("GTA sun shadows: %s (wanted %d, GTA's set %s: %u writes of %u distinct sets this frame, %u writes for other cameras; GTA's atlas on s15: "
					   "%s %ux%u %s)",
					ok ? "on" : "off", want ? 1 : 0, heldShadowOk ? "ok" : "missing", writes, distinct, otherCamera, atlasOk ? "yes" : (t ? "not the atlas" : "none"),
					desc.Width, desc.Height, t ? FormatName(desc.Format, fmt) : "-");
				if (heldShadowOk && (first || cfg.debugShadows || ok)) {
					const auto& r = heldShadow.r;
					for (unsigned i = 0; i < render::shreg::kCount; i += 2) {
						const unsigned j = std::min(i + 1, render::shreg::kCount - 1);
						LC_LOG("  c%u %.6g %.6g %.6g %.6g | c%u %.6g %.6g %.6g %.6g", 53 + i, r[i][0], r[i][1], r[i][2], r[i][3], 53 + j, r[j][0], r[j][1], r[j][2], r[j][3]);
					}
					const auto& ff = shadowwatch::ff;
					LC_LOG("  FusionFix %s: c217 %.3g %.3g %.3g %.3g c218 %.3g %.3g %.3g %.3g c220 %.3g %.3g %.3g %.3g c221 %.3g c223 %.3g %.3g %.3g %.3g",
						ff.known ? "seen" : "not seen", ff.r[0][0], ff.r[0][1], ff.r[0][2], ff.r[0][3], ff.r[1][0], ff.r[1][1], ff.r[1][2], ff.r[1][3], ff.r[3][0],
						ff.r[3][1], ff.r[3][2], ff.r[3][3], ff.r[4][0], ff.r[6][0], ff.r[6][1], ff.r[6][2], ff.r[6][3]);
					if (ok) {
						const auto& p = a_out.params;
						const float l = std::sqrt(p.mz[0] * p.mz[0] + p.mz[1] * p.mz[1] + p.mz[2] * p.mz[2]);
						const float toSun = l > 0.0f ? (p.mz[0] * a_light.sunDir[0] + p.mz[1] * a_light.sunDir[1] + p.mz[2] * a_light.sunDir[2]) / l : 0.0f;
						LC_LOG("  camera %.1f %.1f %.1f forward %.3f %.3f %.3f; cascades end at %.1f %.1f %.1f %.1f m (thresholds %.1f %.1f %.1f), fade %.0f m; light depth "
							   "%.4f per m, along the way to the sun %.3f; softness %.2f bias %.2f blend %.2f fov %.2f taps %d CHSS %d (max %.1f light %.2f) N.L x%.3f %+.3f; "
							   "caster bias %.4f",
							a_f.camPos[0], a_f.camPos[1], a_f.camPos[2], p.fwd[0], p.fwd[1], p.fwd[2], p.bounds[0], p.bounds[1], p.bounds[2], p.bounds[3], p.split[0],
							p.split[1], p.split[2], p.split[3], l, toSun, p.filter[1], p.filter[2], p.filter[3], p.misc[0], p.misc[1] > 0.5f ? 16 : 4,
							p.misc[2] > 0.5f ? 1 : 0, p.chss[0], p.chss[1], p.ndl[0], p.ndl[1], a_out.casterBias);
						for (int k = 0; k < render::kCascades; ++k) {
							LC_LOG("  cascade %d: uv = sc.xy x (%.6g, %.6g) + (%.4f, %.4f)", k, p.casc[k][0], p.casc[k][1], p.casc[k][2], p.casc[k][3]);
						}
						const float at[3] = { p.fwd[0] * 5.0f, p.fwd[1] * 5.0f, p.fwd[2] * 5.0f - 1.5f }, n[3] = { 0.0f, 0.0f, 1.0f };
						const auto  c = render::ShadowLookup(p, at, n);
						LC_LOG("  5 m ahead, 1.5 m down: cascade %d uv %.4f %.4f zr %.6f (taps in u %.3f to %.3f, radius %.6f %.6f)", c.cascade, c.uv[0], c.uv[1], c.zr,
							c.clampU[0], c.clampU[1], c.radius[0], c.radius[1]);
					}
				}
			}
			if (!ok) {
				if (t) {
					t->Release();
				}
				return nullptr;
			}
			return t;
		}

		void LogStats(const FrameSnapshot& a_f)
		{
			const auto now = ::GetTickCount64();
			const bool diag = Config::Get().diagnostics;
			if (now - lastStatsMs < (diag ? 1000u : 10000u)) {
				return;
			}
			const double secs = lastStatsMs ? double(now - lastStatsMs) / 1000.0 : 1.0;
			lastStatsMs = now;
			const auto ws = render::World::Get().TakeStats();
			const auto overlayFrames = Overlay::TakeFrames();
			const auto overlayUploads = Overlay::TakeUploads();
			const auto overlayMs = Overlay::TakeUploadMs();
			const auto frames = std::max(1u, framesDrawn);
			if (ws.drainedBytes || overlayFrames || diag) {
				char        line[600];
				int         n = std::snprintf(line, sizeof(line), "render ring %llu KiB in %.1fs:", static_cast<unsigned long long>(ws.drainedBytes >> 10), secs);
				static const char* kNames[13] = { "pad", "atlas", "section", "clearAll", "texture", "avatar", "scene", "atlasRegion", "lights", "ragdoll",
					"solids", "dug", "liquids" };
				for (int t = 0; t < 13 && n > 0 && n < int(sizeof(line)); ++t) {
					if (ws.messages[t]) {
						n += std::snprintf(line + n, sizeof(line) - n, " %s %u", kNames[t], ws.messages[t]);
					}
				}
				if (ws.otherMessages && n > 0 && n < int(sizeof(line))) {
					n += std::snprintf(line + n, sizeof(line) - n, " unknown %u", ws.otherMessages);
				}
				LC_LOG("%s; overlay frames %u (uploaded %u)", line, overlayFrames, overlayUploads);
			}
			if (diag || ws.frames) {
				LC_LOG("render: %u frames drawn of %u draw commands (%u device lost), %u sections (%.1f MiB, atlas %s %ux%u); per frame: drawn %.0f culled %.0f "
					   "sections, %.0f draw calls, %.0f k tris, draw %.2f ms (worst %.2f), state %.2f ms, ring/uploads %.2f ms, overlay upload %.2f ms",
					framesDrawn, dcCalls, deviceLostFrames, ws.sections, double(ws.sectionBytes) / (1024.0 * 1024.0), ws.atlas ? "yes" : "no", ws.atlasW, ws.atlasH,
					double(ws.sectionsDrawn) / frames, double(ws.sectionsCulled) / frames, double(ws.drawCalls) / frames, double(ws.triangles) / frames / 1000.0,
					ws.drawMs / frames, ws.maxDrawMs, stateMs / frames, ws.drainMs / frames, overlayMs / frames);
				(void)a_f;
			}
			if (ws.bodyFrames) {
				LC_LOG("Minecraft body on Niko's skeleton drawn in %u frames", ws.bodyFrames);
			}
			if (const auto ss = render::shadowpass::TakeStats(); ss.frames || shadowHeldFrames) {
				const double f = ss.frames, g = std::max(1u, ss.gpuFrames);
				LC_LOG("shadows in %u frames (GTA's set from that frame in %u, held from an earlier one in %u; it changed %u times): casters %.0f draws, %.2f ms CPU, "
					   "%.2f ms GPU; GTA's world in the blocks' shadow in %u frames, %.2f ms CPU, %.2f ms GPU (GPU times from %u frames)",
					ss.frames, shadowFreshFrames, shadowHeldFrames, shadowSetChanges, ss.casterDraws / f, ss.casterCpuMs / f, ss.casterGpuMs / g, ss.darkenFrames,
					ss.darkenCpuMs / f, ss.darkenGpuMs / g, ss.gpuFrames);
				shadowFreshFrames = shadowHeldFrames = shadowSetChanges = 0;
			}
			if (mountFrames) {
				LC_LOG("rider at GTA's seat in %u frames: Minecraft's feet were %.2f m behind on average (worst %.2f m)", mountFrames, mountShiftSum / mountFrames,
					mountShiftMax);
			}
			mountFrames = 0;
			mountShiftSum = mountShiftMax = 0.0;
			framesDrawn = dcCalls = deviceLostFrames = 0;
			stateMs = 0.0;
		}

		void OnRenderThread(const FrameSnapshot& a_f)
		{
			if (a_f.magic != FrameSnapshot::kMagic) {
				return;
			}
			IDirect3DDevice9* device = rage::g_pDirect3DDevice;
			if (!device) {
				return;
			}
			++dcCalls;
			if (!renderThreadId) {
				renderThreadId = ::GetCurrentThreadId();
				LC_LOG("first draw command executed: render thread %lu (game thread %lu)", renderThreadId, gameThreadId);
			}
			const HRESULT coop = device->TestCooperativeLevel();
			// Pending render-ring messages first (uploads go to managed resources: fine even when lost).
			render::World::Get().Drain(device);
			const bool overlayWanted = (a_f.flags & render::kFrameDrawOverlay) != 0;
			if (coop != D3D_OK) {
				++deviceLostFrames;
				Overlay::Upload(device, false);
				LogStats(a_f);
				return;
			}

			// The back buffer: what Minecraft's overlay and cursor refer to (for SkyState).
			if (bbCheck++ % 120 == 0) {
				IDirect3DSurface9* bb = nullptr;
				if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
					D3DSURFACE_DESC d{};
					if (SUCCEEDED(bb->GetDesc(&d)) && d.Width && d.Height) {
						if (d.Width != bbW || d.Height != bbH) {
							// The game's own present parameters (it Resets the device on resolution changes).
							D3DPRESENT_PARAMETERS pp{};
							IDirect3DSwapChain9*  sc = nullptr;
							if (SUCCEEDED(device->GetSwapChain(0, &sc)) && sc) {
								sc->GetPresentParameters(&pp);
								sc->Release();
							}
							LC_LOG("back buffer %ux%u (game frame %u; present parameters: %ux%u windowed %d refresh %u Hz, interval 0x%X, window %p)", d.Width,
								d.Height, a_f.gameFrame, pp.BackBufferWidth, pp.BackBufferHeight, pp.Windowed, pp.FullScreen_RefreshRateInHz,
								pp.PresentationInterval, static_cast<void*>(pp.hDeviceWindow));
						}
						bbW = d.Width;
						bbH = d.Height;
						Game::State().viewportW = static_cast<int>(d.Width);
						Game::State().viewportH = static_cast<int>(d.Height);
					}
					bb->Release();
				}
			}

			// Where we are: the bound colour target and depth buffer.
			D3DSURFACE_DESC    rt{}, ds{};
			IDirect3DSurface9* surf = nullptr;
			if (SUCCEEDED(device->GetRenderTarget(0, &surf)) && surf) {
				surf->GetDesc(&rt);
				surf->Release();
			}
			surf = nullptr;
			const bool haveDs = SUCCEEDED(device->GetDepthStencilSurface(&surf)) && surf;
			IDirect3DSurface9* const sceneDs = haveDs ? surf : nullptr;  // compared only (GTA holds it)
			if (haveDs) {
				surf->GetDesc(&ds);
				surf->Release();
			}
			const std::uint32_t key = rt.Width ^ (rt.Height << 12) ^ (ds.Width << 4) ^ (ds.Height << 16) ^ static_cast<std::uint32_t>(ds.Format);
			if (loggedTargets < 6 || (Config::Get().diagnostics && key != lastTargetKey)) {
				++loggedTargets;
				char f1[16], f2[16];
				LC_LOG("draw command (frame %u call %u, thread %lu): render target %ux%u %s ms %u, depth %s %ux%u %s ms %u; camera %s at %.1f %.1f %.1f near %.3f far %.1f",
					a_f.gameFrame, a_f.call, ::GetCurrentThreadId(), rt.Width, rt.Height, FormatName(rt.Format, f1), static_cast<unsigned>(rt.MultiSampleType),
					haveDs ? "bound" : "NONE", ds.Width, ds.Height, haveDs ? FormatName(ds.Format, f2) : "-", static_cast<unsigned>(ds.MultiSampleType),
					SourceName(a_f.source), a_f.camPos[0], a_f.camPos[1], a_f.camPos[2], a_f.nearZ, a_f.farZ);
			}
			lastTargetKey = key;

			// Draw once per game frame, into the frame's full-size target.
			const bool fullSize = rt.Width && rt.Height && (!bbW || (rt.Width == bbW && rt.Height == bbH));
			const bool drawWorld = (a_f.flags & render::kFrameDrawWorld) != 0;
			if (!sunwatch::tried) {
				sunwatch::Install(device);  // from the first frame on, so the first lit frame has GTA's values
			}
			if (!drawWorld) {
				sunwatch::Reset();  // no blocks to light: the next frame's scene starts afresh
				shadowwatch::Reset();
			}
			if (a_f.gameFrame == drawnFrame || !fullSize || !(drawWorld || overlayWanted)) {
				render::opaquedepth::AtDrawCommand(device, sceneDs, false, false);
				Overlay::Upload(device, false);
				LogStats(a_f);
				return;
			}
			drawnFrame = a_f.gameFrame;
			Overlay::Upload(device, overlayWanted);

			render::TargetInfo target;
			target.width = rt.Width;
			target.height = rt.Height;
			target.depthTest = (a_f.flags & render::kFrameDepthTest) && haveDs && ds.Width == rt.Width && ds.Height == rt.Height &&
			                   ds.MultiSampleType == rt.MultiSampleType;
			// GTA's depth from before its transparent pass (render/OpaqueDepth.h): copied in GTA's frame, put back below.
			render::opaquedepth::AtDrawCommand(device, sceneDs, true, drawWorld && target.depthTest);

			const double       t0 = NowMs();
			IDirect3DStateBlock9* saved = nullptr;
			if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) {
				LC_LOG_EVERY(5000, "ERROR: CreateStateBlock failed: not drawing");
				return;
			}
			stateMs += NowMs() - t0;
			sunwatch::ours = true;  // from here to the state block's Apply the constants written are ours
			render::opaquedepth::SetOurs(true);
			if (drawWorld) {
				const render::LightingParams light = GtaLighting(device, a_f, rt.Width, rt.Height, target.adaptedLum);
				// The mount: where Minecraft's latest scene has the rider's feet.
				proto::McState mc{};
				double         mountFrom[3]{};
				const bool     mount = (a_f.flags & render::kFrameMountShift) && Link::Get().ReadMcState(mc) && (mc.flags & proto::kMcInWorld);
				if (mount) {
					mountFrom[0] = mc.x;
					mountFrom[1] = mc.y;
					mountFrom[2] = mc.z;
					const double dx = a_f.feet[0] - mc.x, dy = a_f.feet[1] - mc.y, dz = a_f.feet[2] - mc.z;
					const double shift = std::sqrt(dx * dx + dy * dy + dz * dz);
					++mountFrames;
					mountShiftSum += shift;
					mountShiftMax = std::max(mountShiftMax, shift);
				}
				render::ShadowFrame    shadowFrame;
				IDirect3DBaseTexture9* gtaAtlas = GtaShadows(device, a_f, light, rt.Width, rt.Height, shadowFrame);
				// After GTA's bound textures were read above: GTA's glass out of its depth buffer.
				if (target.depthTest) {
					render::opaquedepth::Restore(device, rt.Width, rt.Height);
				}
				render::World::Get().Draw(device, a_f, target, light, mount ? mountFrom : nullptr, gtaAtlas ? &shadowFrame : nullptr);
				if (gtaAtlas) {
					gtaAtlas->Release();
				}
			}
			if (overlayWanted) {
				Overlay::Draw(device, a_f, rt.Width, rt.Height);
			}
			const double t1 = NowMs();
			saved->Apply();
			saved->Release();
			sunwatch::ours = false;
			render::opaquedepth::SetOurs(false);
			stateMs += NowMs() - t1;
			++framesDrawn;
			LogStats(a_f);
		}
	}

	void Draw()
	{
		Perf::Scope timer(Perf::kDraw);
		const auto& cfg = Config::Get();
		if (!scanned) {
			gameThreadId = ::GetCurrentThreadId();
			ScanAddresses();
		}
		const std::uint32_t gameFrame = CTimer::m_FrameCounter;
		callInFrame = gameFrame == lastGameFrame ? callInFrame + 1 : 0;
		lastGameFrame = gameFrame;

		if (!cfg.render) {
			// Nothing is drawn; keep the ring moving (World never touches a device here).
			render::World::Get().Drain(nullptr);
			return;
		}
		if (!rage::g_pDirect3DDevice) {
			return;
		}
		FrameSnapshot f;
		f.gameFrame = gameFrame;
		f.call = callInFrame;
		Capture(f, cfg);
		if (f.call == 0) {
			DebugSequence(cfg, (f.flags & render::kFrameDrawWorld) != 0);
			DebugSpot(cfg, (f.flags & render::kFrameDrawWorld) != 0);
			DebugVehicleSpeed(cfg);
		}
		Enqueue(f);
	}
}

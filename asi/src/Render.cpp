// Unity-built into dllmain.cpp (needs IV-SDK). See Render.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "render"
#include "Render.h"

#include "render/Frame.h"
#include "render/RenderMath.h"
#include "render/World.h"

#include "Config.h"
#include "Coords.h"
#include "Game.h"
#include "Link.h"
#include "Log.h"
#include "Overlay.h"
#include "Perf.h"

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

			// Lighting: GTA's clock.
			a_f.gameHour = float(CClock::ms_nGameClockHours) + float(CClock::ms_nGameClockMinutes) / 60.0f + float(CClock::ms_nGameClockSeconds) / 3600.0f;
			a_f.dayFactor = render::DayFactor(a_f.gameHour);
			a_f.exposure = a_cfg.renderExposure > 0.0f ? a_cfg.renderExposure : 1.0f;

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
			if (alive && !menu && ok) {
				a_f.flags |= render::kFrameDrawWorld;
			}
			using OM = Config::OverlayMode;
			if (alive && haveMc && !menu && a_cfg.overlay != OM::kOff && (puppeting || screenOpen || a_cfg.overlay == OM::kAlways)) {
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
			if (inWorld) {
				a_f.flags |= render::kFrameAvatar;
				a_f.feet[0] = mc.x;
				a_f.feet[1] = mc.y;
				a_f.feet[2] = mc.z;
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
				static const char* kNames[12] = { "pad", "atlas", "section", "clearAll", "texture", "avatar", "scene", "atlasRegion", "lights", "ragdoll",
					"solids", "dug" };
				for (int t = 0; t < 12 && n > 0 && n < int(sizeof(line)); ++t) {
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
			if (a_f.gameFrame == drawnFrame || !fullSize || !(drawWorld || overlayWanted)) {
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

			const double       t0 = NowMs();
			IDirect3DStateBlock9* saved = nullptr;
			if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) {
				LC_LOG_EVERY(5000, "ERROR: CreateStateBlock failed: not drawing");
				return;
			}
			stateMs += NowMs() - t0;
			if (drawWorld) {
				render::World::Get().Draw(device, a_f, target);
			}
			if (overlayWanted) {
				Overlay::Draw(device, a_f, rt.Width, rt.Height);
			}
			const double t1 = NowMs();
			saved->Apply();
			saved->Release();
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
		Enqueue(f);
	}
}

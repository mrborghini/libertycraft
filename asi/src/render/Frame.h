// What the game thread hands the render thread for one frame: the camera the game renders this
// frame with, the lighting inputs and what to draw. Captured in drawingEvent (game thread, while
// the render phases build their draw lists) and carried to the render thread inside our draw
// command, so the blocks use exactly the camera of the frame they're drawn in.
#pragma once

#include <cstdint>

namespace lc::render
{
	enum FrameFlags : std::uint32_t
	{
		kFrameCameraValid = 1u << 0,   // clip/camPos are usable
		kFrameDrawWorld = 1u << 1,     // Minecraft's blocks and entities
		kFrameDrawOverlay = 1u << 2,   // Minecraft's GUI/HUD
		kFrameLogDepth = 1u << 3,      // the depth buffer holds FusionFix's logarithmic depth
		kFrameDepthTest = 1u << 4,     // test against the bound depth buffer
		kFramePuppeting = 1u << 5,
		kFrameScreenOpen = 1u << 6,    // a Minecraft screen: draw its cursor
		kFrameCrosshair = 1u << 7,     // Minecraft shows its crosshair: the invert pass
		kFrameAvatar = 1u << 8,        // the player's third-person body at feet
		kFrameMountShift = 1u << 9,    // in a vehicle: feet = GTA's seat this frame; the mount moves there too
		kFrameGtaLighting = 1u << 10,  // light with GTA's sun, ambient and fog (else Minecraft's own lighting)
		kFrameBody = 1u << 11,         // the player's body posed on GTA's skeleton (bodyParts at bodyOrigin; Body.h)
	};

	enum class CameraSource : std::uint32_t
	{
		kNone = 0,
		kPhaseViewport = 1,    // CRenderPhase::sm_pCurrent's grcViewport (view + projection matrices)
		kCurrentViewport = 2,  // grcViewport::sm_pCurrent
		kFinalCam = 3,         // TheCamera.m_pFinalCam (matrix rows + vertical FOV)
	};

	struct FrameSnapshot
	{
		static constexpr std::uint32_t kMagic = 0x4D52464C;  // "LFRM"
		std::uint32_t magic = kMagic;
		std::uint32_t gameFrame = 0;   // CTimer::m_FrameCounter
		std::uint32_t call = 0;        // drawingEvent call of this game frame (it runs twice in game)
		std::uint32_t flags = 0;       // FrameFlags
		double        camPos[3]{};     // GTA world metres
		float         clip[4][4]{};    // camera-relative GTA -> clip, row vectors
		float         nearZ = 0.1f, farZ = 1000.0f;
		float         dayFactor = 1.0f;
		float         gameHour = 12.0f;
		double        feet[3]{};       // Minecraft feet (avatar), MC coords; with kFrameMountShift GTA's seat
		float         rain = 0.0f;     // CWeather::Rain (0..1)
		std::int32_t  cursorX = 0, cursorY = 0;  // overlay pixels
		std::uint32_t guiScale = 0;
		CameraSource  source = CameraSource::kNone;
		float         exposure = 1.0f;
		std::uint32_t viewportW = 0, viewportH = 0;  // the camera's grcViewport size, when known
		std::uint32_t pad = 0;
		// kFrameBody: per RagdollPart, the standing body -> GTA metres relative to bodyOrigin (GTA world).
		double        bodyOrigin[3]{};
		float         bodyParts[7][3][4]{};
	};
}

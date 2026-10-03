// GTA IV <-> Minecraft coordinates and angles. Header-only and SDK-free (unit-testable).
//
// Axes. GTA IV: x east, y north, z up, metres. Minecraft: x east, y up, z south, blocks.
//   mc = (x, z, -y)     gta = (x, -z, y)     1 block = 1 metre
// (a proper rotation, determinant +1: both systems are right-handed).
//
// Yaw. GTA heading h (degrees): 0 = facing north (+y), increasing counter-clockwise seen from
// above, so 90 = west. A ped facing h looks along (-sin h, cos h, 0); CVector::Heading() is
// atan2(-x, y). Minecraft yaw (degrees): 0 = facing south (+z), increasing clockwise, 90 = west;
// it looks along mc (-sin yaw, 0, cos yaw) = gta (-sin yaw, -cos yaw, 0).
// Equal directions need sin h = sin yaw and cos h = -cos yaw, so
//   h = 180 - yaw     yaw = 180 - h
// (a reflection: the two turn opposite ways). If it turns out mirrored in game, flip
// kHeadingSign / kHeadingOffset below; nothing else hard-codes the relation.
//
// Pitch. Minecraft pitch: positive = looking down. GTA camera rotation x (SET_CAM_ROT):
// positive = looking up. gtaPitch = -mcPitch.
#pragma once

#include <cmath>

namespace lc
{
	struct McVec
	{
		double x, y, z;
	};

	struct GtaVec
	{
		double x, y, z;
	};

	inline McVec GtaToMc(double a_x, double a_y, double a_z) { return { a_x, a_z, -a_y }; }
	inline McVec GtaToMc(const GtaVec& a_p) { return GtaToMc(a_p.x, a_p.y, a_p.z); }
	inline GtaVec McToGta(double a_x, double a_y, double a_z) { return { a_x, -a_z, a_y }; }
	inline GtaVec McToGta(const McVec& a_p) { return McToGta(a_p.x, a_p.y, a_p.z); }

	inline constexpr float kPi = 3.14159265358979f;
	inline constexpr float kDegToRad = kPi / 180.0f;
	inline constexpr float kRadToDeg = 180.0f / kPi;

	// The one place the heading <-> yaw relation lives: h = kHeadingOffset + kHeadingSign * yaw.
	inline constexpr float kHeadingSign = -1.0f;
	inline constexpr float kHeadingOffset = 180.0f;

	inline float WrapDegrees(float a_deg)  // -> [0, 360)
	{
		float d = std::fmod(a_deg, 360.0f);
		return d < 0.0f ? d + 360.0f : d;
	}

	inline float WrapDegrees180(float a_deg)  // -> [-180, 180)
	{
		return WrapDegrees(a_deg + 180.0f) - 180.0f;
	}

	inline float McYawToGtaHeading(float a_yaw) { return WrapDegrees(kHeadingOffset + kHeadingSign * a_yaw); }
	inline float GtaHeadingToMcYaw(float a_heading) { return WrapDegrees180((a_heading - kHeadingOffset) / kHeadingSign); }
	inline float McPitchToGtaPitch(float a_pitch) { return -a_pitch; }
	inline float GtaPitchToMcPitch(float a_pitch) { return -a_pitch; }

	// Camera basis in GTA space for a Minecraft look direction (degrees). forward is where the
	// camera looks, right/up complete a right-handed frame (right x forward = up, z-up world).
	struct GtaBasis
	{
		float right[3], forward[3], up[3];
	};

	inline GtaBasis LookBasis(float a_mcYaw, float a_mcPitch)
	{
		const float h = McYawToGtaHeading(a_mcYaw) * kDegToRad;
		const float p = McPitchToGtaPitch(a_mcPitch) * kDegToRad;  // up positive
		const float sh = std::sin(h), ch = std::cos(h), sp = std::sin(p), cp = std::cos(p);
		GtaBasis b{};
		// heading h faces (-sin h, cos h); pitched up by p
		b.forward[0] = -sh * cp;
		b.forward[1] = ch * cp;
		b.forward[2] = sp;
		// right = forward x up(world) normalised, horizontal: (cos h, sin h, 0)
		b.right[0] = ch;
		b.right[1] = sh;
		b.right[2] = 0.0f;
		// up = right x forward
		b.up[0] = b.right[1] * b.forward[2] - b.right[2] * b.forward[1];
		b.up[1] = b.right[2] * b.forward[0] - b.right[0] * b.forward[2];
		b.up[2] = b.right[0] * b.forward[1] - b.right[1] * b.forward[0];
		return b;
	}
}

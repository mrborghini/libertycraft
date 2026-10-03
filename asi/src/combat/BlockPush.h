// Minecraft blocks stop GTA IV's peds and vehicles: the math (SDK-free, unit-tested on Linux by
// tests/combat_test.cpp). NpcBlocks.cpp applies it to the game's peds and vehicles each frame.
//
// Minecraft sends, per 16x16x16 section, which of its blocks NPCs collide with (proto::kRenSolids,
// a 512-byte bitset, bit x + 16z + 256y). Everything here is in Minecraft space (blocks; x east,
// y up, z south) and only looks at the blocks from above: a ped is a circle, a vehicle a row of
// circles along its axis, and a block column is a solid square if any of its blocks in the body's
// height range is solid (SkyCraft's NpcBlocks.cpp did the same for Skyrim's NPCs).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace lc::blocks
{
	class SolidGrid
	{
	public:
		using Bits = std::array<std::uint64_t, 64>;

		static std::uint64_t Key(std::int32_t a_sx, std::int32_t a_sy, std::int32_t a_sz)
		{
			return (std::uint64_t(std::uint32_t(a_sx) & 0x3FFFFF) << 42) | (std::uint64_t(std::uint32_t(a_sz) & 0x3FFFFF) << 20) |
			       std::uint64_t(std::uint32_t(a_sy) & 0xFFFFF);
		}

		// a_bits: the section's 512-byte bitset, or null (or all clear) to drop the section.
		void Set(std::int32_t a_sx, std::int32_t a_sy, std::int32_t a_sz, const std::uint8_t* a_bits)
		{
			const auto key = Key(a_sx, a_sy, a_sz);
			++generation_;
			cacheKey_ = ~0ull;
			Bits bits{};
			if (a_bits) {
				std::memcpy(bits.data(), a_bits, sizeof(Bits));
			}
			if (std::all_of(bits.begin(), bits.end(), [](std::uint64_t w) { return w == 0; })) {
				sections_.erase(key);
				return;
			}
			sections_[key] = bits;
		}

		void Clear()
		{
			++generation_;
			cacheKey_ = ~0ull;
			sections_.clear();
		}

		[[nodiscard]] bool Solid(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z) const
		{
			const auto key = Key(a_x >> 4, a_y >> 4, a_z >> 4);  // arithmetic shift: floor for negatives too
			if (key != cacheKey_) {
				const auto it = sections_.find(key);
				cacheKey_ = key;
				cacheBits_ = it == sections_.end() ? nullptr : &it->second;
			}
			if (!cacheBits_) {
				return false;
			}
			const int bit = (a_x & 15) + 16 * (a_z & 15) + 256 * (a_y & 15);
			return ((*cacheBits_)[bit >> 6] >> (bit & 63)) & 1u;
		}

		// Any solid block in column (a_x, a_z) from row a_y0 to a_y1 (inclusive).
		[[nodiscard]] bool ColumnBlocked(std::int32_t a_x, std::int32_t a_z, std::int32_t a_y0, std::int32_t a_y1) const
		{
			for (std::int32_t y = a_y0; y <= a_y1; ++y) {
				if (Solid(a_x, y, a_z)) {
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] bool Empty() const { return sections_.empty(); }
		[[nodiscard]] std::size_t Sections() const { return sections_.size(); }
		[[nodiscard]] std::uint32_t Generation() const { return generation_; }

	private:
		std::unordered_map<std::uint64_t, Bits> sections_;
		std::uint32_t                           generation_ = 0;
		mutable std::uint64_t                   cacheKey_ = ~0ull;
		mutable const Bits*                     cacheBits_ = nullptr;
	};

	// Rows a body standing at a_feetY with height a_height overlaps: from a little above the feet
	// (standing on a block, or a block sunk into GTA's ground, is fine) to the head.
	inline void BodyRows(double a_feetY, double a_height, double a_lift, std::int32_t& a_y0, std::int32_t& a_y1)
	{
		a_y0 = static_cast<std::int32_t>(std::floor(a_feetY + a_lift));
		a_y1 = static_cast<std::int32_t>(std::floor(a_feetY + std::max(a_height, a_lift + 0.01) - 0.05));
	}

	// One pass of a circle (centre a_x, a_z, radius a_r) against the blocked columns: the push out of
	// the squares it overlaps, along the sum of their pushes and as long as the deepest one (a flat
	// wall made of several columns pushes once, not once per column). False if it overlaps none.
	inline bool CirclePush(const SolidGrid& a_grid, double a_x, double a_z, double a_r, std::int32_t a_y0, std::int32_t a_y1, double& a_px, double& a_pz)
	{
		a_px = a_pz = 0.0;
		bool   any = false;
		double deepest = 0.0;
		auto   add = [&](double a_dx, double a_dz, double a_depth) {
			a_px += a_dx * a_depth;
			a_pz += a_dz * a_depth;
			deepest = std::max(deepest, a_depth);
		};
		for (auto bx = static_cast<std::int32_t>(std::floor(a_x - a_r)); bx <= static_cast<std::int32_t>(std::floor(a_x + a_r)); ++bx) {
			for (auto bz = static_cast<std::int32_t>(std::floor(a_z - a_r)); bz <= static_cast<std::int32_t>(std::floor(a_z + a_r)); ++bz) {
				if (!a_grid.ColumnBlocked(bx, bz, a_y0, a_y1)) {
					continue;
				}
				const double qx = std::clamp(a_x, double(bx), double(bx + 1)), qz = std::clamp(a_z, double(bz), double(bz + 1));
				double       dx = a_x - qx, dz = a_z - qz;
				const double d = std::sqrt(dx * dx + dz * dz);
				if (d >= a_r) {
					continue;
				}
				any = true;
				if (d < 1e-6) {
					// Centre inside the square: out through its nearest side.
					const double toW = a_x - bx, toE = bx + 1 - a_x, toN = a_z - bz, toS = bz + 1 - a_z;
					const double m = std::min({ toW, toE, toN, toS });
					dx = m == toW ? -1.0 : m == toE ? 1.0 : 0.0;
					dz = dx != 0.0 ? 0.0 : m == toN ? -1.0 : 1.0;
					add(dx, dz, m + a_r);
				} else {
					add(dx / d, dz / d, a_r - d);
				}
			}
		}
		const double len = std::sqrt(a_px * a_px + a_pz * a_pz);
		if (any && len > 1e-9) {
			a_px *= deepest / len;
			a_pz *= deepest / len;
		}
		return any;
	}

	// Moves a circle out of the blocked columns (up to 3 passes, for corners). True if it moved.
	inline bool PushCircleOut(const SolidGrid& a_grid, double& a_x, double& a_z, double a_r, std::int32_t a_y0, std::int32_t a_y1)
	{
		bool moved = false;
		for (int pass = 0; pass < 3; ++pass) {
			double px = 0.0, pz = 0.0;
			if (!CirclePush(a_grid, a_x, a_z, a_r, a_y0, a_y1, px, pz) || (std::abs(px) < 1e-4 && std::abs(pz) < 1e-4)) {
				break;
			}
			a_x += px;
			a_z += pz;
			moved = true;
		}
		return moved;
	}

	// How far (blocks, up to a_max) along (a_tx, a_tz) from (a_cx, a_cz) until the wall on the side
	// of normal (-a_nx, -a_nz) ends, so a ped following it can turn the corner there; a_max + 1 if it
	// doesn't end (or that way is walled off too).
	inline double WallEnd(const SolidGrid& a_grid, double a_cx, double a_cz, double a_tx, double a_tz, double a_nx, double a_nz, std::int32_t a_y0,
		std::int32_t a_y1, double a_max)
	{
		for (double s = 0.5; s <= a_max; s += 0.5) {
			const double px = a_cx + a_tx * s, pz = a_cz + a_tz * s;
			if (a_grid.ColumnBlocked(static_cast<std::int32_t>(std::floor(px)), static_cast<std::int32_t>(std::floor(pz)), a_y0, a_y1)) {
				return a_max + 1.0;
			}
			const double wx = px - a_nx, wz = pz - a_nz;  // one block into the wall's side
			if (!a_grid.ColumnBlocked(static_cast<std::int32_t>(std::floor(wx)), static_cast<std::int32_t>(std::floor(wz)), a_y0, a_y1)) {
				return s;
			}
		}
		return a_max + 1.0;
	}

	// A ped walking into a wall (pushed back against where it's heading) follows the wall instead:
	// the direction along it (+1 or -1 times the tangent (-nz, nx)) whose end is nearer the way it
	// was going. a_nx/a_nz: the push out of the wall (unit); a_gx/a_gz: where the ped heads (unit).
	// 0: it isn't walking into the wall.
	inline int DetourSide(const SolidGrid& a_grid, double a_cx, double a_cz, double a_nx, double a_nz, double a_gx, double a_gz, std::int32_t a_y0,
		std::int32_t a_y1)
	{
		if (a_gx * -a_nx + a_gz * -a_nz <= 0.2) {
			return 0;
		}
		const double tx = -a_nz, tz = a_nx;
		const double endA = WallEnd(a_grid, a_cx, a_cz, tx, tz, a_nx, a_nz, a_y0, a_y1, 12.0);
		const double endB = WallEnd(a_grid, a_cx, a_cz, -tx, -tz, a_nx, a_nz, a_y0, a_y1, 12.0);
		const double along = a_gx * tx + a_gz * tz;  // which end is toward where it was going
		return (endA - 2.0 * along) <= (endB + 2.0 * along) ? 1 : -1;
	}

	// ---- vehicles ----------------------------------------------------------------------------------
	// A vehicle seen from above: centre (a_cx, a_cz), unit forward (a_fx, a_fz), half length and half
	// width. Its body is a row of circles of radius a_halfWidth along the axis (the ends are rounded:
	// a wall still stops it a few centimetres later). The contact: normal = out of the blocks
	// (horizontal, unit), depth = how far the deepest circle is in.
	struct Contact
	{
		bool   hit = false;
		double nx = 0.0, nz = 0.0;
		double depth = 0.0;
	};

	inline Contact VehicleContact(const SolidGrid& a_grid, double a_cx, double a_cz, double a_fx, double a_fz, double a_halfLength, double a_halfWidth,
		std::int32_t a_y0, std::int32_t a_y1)
	{
		Contact c;
		const double r = std::max(a_halfWidth, 0.2);
		const double reach = std::max(a_halfLength - r, 0.0);
		const int    n = 1 + static_cast<int>(std::ceil(2.0 * reach / r));
		double       sx = 0.0, sz = 0.0;
		for (int i = 0; i < n; ++i) {
			const double t = n == 1 ? 0.0 : -reach + 2.0 * reach * double(i) / double(n - 1);
			double       px = 0.0, pz = 0.0;
			if (!CirclePush(a_grid, a_cx + a_fx * t, a_cz + a_fz * t, r, a_y0, a_y1, px, pz)) {
				continue;
			}
			const double len = std::sqrt(px * px + pz * pz);
			if (len < 1e-6) {
				continue;
			}
			c.hit = true;
			c.depth = std::max(c.depth, len);
			sx += px;
			sz += pz;
		}
		const double len = std::sqrt(sx * sx + sz * sz);
		if (!c.hit || len < 1e-9) {
			return Contact{};
		}
		c.nx = sx / len;
		c.nz = sz / len;
		return c;
	}

	// The velocity (a_vx, a_vz) after meeting a surface with normal (a_nx, a_nz) (out of the blocks,
	// unit): what goes into the blocks is taken out and a_bounce of it sent back. True if it changed.
	inline bool BlockVelocity(double& a_vx, double& a_vz, double a_nx, double a_nz, double a_bounce)
	{
		const double vn = a_vx * a_nx + a_vz * a_nz;
		if (vn >= 0.0) {
			return false;
		}
		a_vx -= (1.0 + a_bounce) * vn * a_nx;
		a_vz -= (1.0 + a_bounce) * vn * a_nz;
		return true;
	}
}

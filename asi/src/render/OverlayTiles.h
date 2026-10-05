// The overlay's dirty tiles (the protocol's kOverlayFlagTiles): which parts of Minecraft's overlay
// frame changed since the frame the overlay texture holds. SDK-free (tests/overlay_test.cpp).
#pragma once

#include "libertycraft_protocol.h"

#include <cstdint>
#include <cstring>

namespace lc::render::overlaytiles
{
	inline constexpr std::uint32_t kGrid = libertycraft::proto::kOverlayTileGrid;
	inline constexpr std::uint32_t kWords = kGrid * kGrid / 32;

	// Tile (tx, ty) is bit (ty * 16 + tx) % 32 of w[(ty * 16 + tx) / 32].
	struct Mask
	{
		std::uint32_t w[kWords]{};

		bool Any() const
		{
			for (auto v : w) {
				if (v) {
					return true;
				}
			}
			return false;
		}
		bool Get(std::uint32_t a_tile) const { return (w[a_tile >> 5] >> (a_tile & 31)) & 1u; }
		void Set(std::uint32_t a_tile) { w[a_tile >> 5] |= 1u << (a_tile & 31); }
		void Or(const Mask& a_o)
		{
			for (std::uint32_t i = 0; i < kWords; ++i) {
				w[i] |= a_o.w[i];
			}
		}
		void Clear() { std::memset(w, 0, sizeof(w)); }
		std::uint32_t Count() const
		{
			std::uint32_t n = 0;
			for (auto v : w) {
				for (; v; v &= v - 1) {
					++n;
				}
			}
			return n;
		}
	};

	inline std::uint32_t TileW(std::uint32_t a_w) { return (a_w + kGrid - 1) / kGrid; }
	inline std::uint32_t TileH(std::uint32_t a_h) { return (a_h + kGrid - 1) / kGrid; }

	inline Mask HeaderTiles(const libertycraft::proto::OverlaySlotHdr& a_h)
	{
		Mask m;
		std::memcpy(m.w, reinterpret_cast<const std::uint8_t*>(&a_h) + libertycraft::proto::kOverlayHdrTilesOff, sizeof(m.w));
		return m;
	}

	inline std::uint64_t HeaderBase(const libertycraft::proto::OverlaySlotHdr& a_h)
	{
		std::uint64_t v = 0;
		std::memcpy(&v, reinterpret_cast<const std::uint8_t*>(&a_h) + libertycraft::proto::kOverlayHdrBaseFrameOff, sizeof(v));
		return v;
	}

	// The reader's side (render thread): what its copy (the texture) lacks of the frame acquired last.
	struct Reader
	{
		bool          covered = false;  // pending says how the copy differs from frame coveredTo
		std::uint64_t coveredTo = 0;
		std::uint32_t w = 0, h = 0;     // of frame coveredTo
		bool          all = true;       // the copy needs the whole frame
		Mask          pending;

		// The copy is gone or stale in a way nobody tracked (a new texture, a new device).
		void Reset()
		{
			all = true;
			pending.Clear();
		}

		// A frame was just acquired (its slot header).
		void Acquired(const libertycraft::proto::OverlaySlotHdr& a_hdr)
		{
			const bool tiles = (a_hdr.flags & libertycraft::proto::kOverlayFlagTiles) && covered && HeaderBase(a_hdr) == coveredTo && a_hdr.width == w &&
			                   a_hdr.height == h;
			if (tiles) {
				pending.Or(HeaderTiles(a_hdr));
			} else {
				all = true;
			}
			covered = true;
			coveredTo = a_hdr.frameId;
			w = a_hdr.width;
			h = a_hdr.height;
		}

		// The copy now holds the frame acquired last.
		void Uploaded()
		{
			all = false;
			pending.Clear();
		}
	};

	struct Rect
	{
		std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // pixels, x1 / y1 exclusive
	};

	// The pixel bounds of the mask's tiles in a w x h frame (false: none).
	inline bool Bounds(const Mask& a_m, std::uint32_t a_w, std::uint32_t a_h, Rect& a_out)
	{
		const std::uint32_t tw = TileW(a_w), th = TileH(a_h);
		bool                any = false;
		std::uint32_t       tx0 = kGrid, ty0 = kGrid, tx1 = 0, ty1 = 0;
		for (std::uint32_t t = 0; t < kGrid * kGrid; ++t) {
			if (!a_m.Get(t)) {
				continue;
			}
			const std::uint32_t tx = t % kGrid, ty = t / kGrid;
			if (tx * tw >= a_w || ty * th >= a_h) {
				continue;  // past a short frame's last tile
			}
			any = true;
			tx0 = tx < tx0 ? tx : tx0;
			ty0 = ty < ty0 ? ty : ty0;
			tx1 = tx > tx1 ? tx : tx1;
			ty1 = ty > ty1 ? ty : ty1;
		}
		if (!any) {
			return false;
		}
		a_out.x0 = tx0 * tw;
		a_out.y0 = ty0 * th;
		a_out.x1 = (tx1 + 1) * tw < a_w ? (tx1 + 1) * tw : a_w;
		a_out.y1 = (ty1 + 1) * th < a_h ? (ty1 + 1) * th : a_h;
		return true;
	}

	// Calls a_fn(x0, x1, y0, y1) (pixels, exclusive ends) for each run of neighbouring set tiles of a tile row.
	template <class Fn>
	void ForEachSpan(const Mask& a_m, std::uint32_t a_w, std::uint32_t a_h, Fn&& a_fn)
	{
		const std::uint32_t tw = TileW(a_w), th = TileH(a_h);
		for (std::uint32_t ty = 0; ty < kGrid && ty * th < a_h; ++ty) {
			const std::uint32_t y0 = ty * th, y1 = y0 + th < a_h ? y0 + th : a_h;
			for (std::uint32_t tx = 0; tx < kGrid && tx * tw < a_w;) {
				if (!a_m.Get(ty * kGrid + tx)) {
					++tx;
					continue;
				}
				std::uint32_t end = tx;
				while (end + 1 < kGrid && (end + 1) * tw < a_w && a_m.Get(ty * kGrid + end + 1)) {
					++end;
				}
				const std::uint32_t x0 = tx * tw, x1 = (end + 1) * tw < a_w ? (end + 1) * tw : a_w;
				a_fn(x0, x1, y0, y1);
				tx = end + 1;
			}
		}
	}
}

// The shared-memory link to the Minecraft mod (protocol/libertycraft_protocol.h), host side.
// Port of SkyCraft's Link. The ring/seqlock logic is in LinkCore.h; this file owns the mapping.
//
// Transport under Wine/Proton: the plain file Z:\dev\shm\libertycraft-bridge (the Linux side's
// /dev/shm/libertycraft-bridge) opened with CreateFileW(OPEN_ALWAYS) + CreateFileMappingW
// (PAGE_READWRITE): Wine maps it MAP_SHARED, so both processes see the same pages. The file is
// only ever grown to kMappingBytes, never shrunk (the other side has it mapped: SIGBUS).
// Without Z:\dev\shm (native Windows) it falls back to the named section Local\LibertyCraft_v1.
//
// The ~191 MiB mapping is mapped as three views (control + rings, overlay pixels, render ring)
// so a fragmented 32-bit address space only has to find ~64 MiB in one piece; the overlay
// view is optional (logged; the overlay stays off without it).
//
// Lifetime: a background thread beats the host heartbeat every 250 ms (also through loading
// screens, the pause menu and alt-tab) and, while Minecraft's heartbeat is missing, checks once
// a second that the bridge file is still the one we mapped. Minecraft deletes it when it exits
// and sees a stale host; if it's gone or replaced, the link reopens it (Generation() bumps).
// Every accessor holds a shared lock, the reopen an exclusive one, so no view disappears under a
// reader on another thread.
#pragma once

#include "LinkCore.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

namespace lc
{
	namespace proto = ::libertycraft::proto;

	class Link
	{
	public:
		static Link& Get();

		// Opens/creates the mapping and resets the host's regions. Idempotent and throttled
		// (one attempt every 2 s while it fails); logs the outcome.
		bool Create();
		void Destroy();
		[[nodiscard]] bool Valid() const { return ready_.load(std::memory_order_acquire); }
		// Bumps every time the mapping is (re)created: the game must treat it like a reconnect.
		[[nodiscard]] std::uint32_t Generation() const { return generation_.load(std::memory_order_acquire); }

		// Starts the heartbeat/watchdog thread (also creates the link if it isn't yet).
		void StartHeartbeatThread();

		// ---- header ------------------------------------------------------------------------------
		void Heartbeat();
		// Minecraft's heartbeat is younger than 3000 ms.
		[[nodiscard]] bool          MinecraftAlive() const;
		[[nodiscard]] std::uint64_t McHeartbeatAgeMs() const;  // UINT64_MAX: never
		[[nodiscard]] std::uint32_t McPid() const;

		// ---- seqlocked blocks --------------------------------------------------------------------
		void WriteSkyState(const proto::SkyState& a_state);
		void WriteWaterGrid(const proto::WaterGrid& a_grid);
		void WriteActors(const proto::ActorRecord* a_records, std::uint32_t a_count);
		bool ReadMcState(proto::McState& a_out) const;
		bool ReadWorldEntities(proto::WorldEntities& a_out) const;

		// ---- rings -------------------------------------------------------------------------------
		// Input ring producer. Thread-safe (window procedure + game thread). False: dropped.
		bool PushInput(proto::InputType a_type, std::uint16_t a_code, std::int32_t a_a = 0, std::int32_t a_b = 0, std::int32_t a_c = 0);
		// Event ring consumer (one thread). False when empty.
		bool PopEvent(proto::McEvent& a_out);
		// Collision ring producer (one thread). False when full or too large (logged).
		bool WriteCollision(proto::ColType a_type, const void* a_payload, std::uint32_t a_bytes);
		[[nodiscard]] std::uint64_t CollisionPending() const;
		// Render ring consumer (one thread): a_fn(type, payload, bytes) per message, up to about
		// a_maxBytes. The payload points into shared memory and is valid only inside a_fn.
		// Returns bytes consumed.
		template <class Fn>
		std::uint64_t DrainRender(Fn&& a_fn, std::uint64_t a_maxBytes)
		{
			std::shared_lock lock(rw_);
			if (!render_) {
				return 0;
			}
			return core::ByteRing{ render_, proto::kRenRingDataBytes }.Drain(a_fn, a_maxBytes);
		}

		// ---- overlay triple buffer (consumer, render thread) --------------------------------------
		// If Minecraft published a newer frame, makes it the front slot and returns true.
		bool AcquireOverlayFrame();
		// Minecraft (re)connected: its writer restarts at slot 1, so restart the swap too.
		void ResetOverlay();
		[[nodiscard]] bool          OverlayMapped() const { return overlayMapped_.load(std::memory_order_acquire); }
		[[nodiscard]] std::uint32_t FrontSlot() const { return overlayFront_; }
		// a_fn(header, pixels) on the current front slot under the link's lock; false if unmapped.
		template <class Fn>
		bool WithFrontSlot(Fn&& a_fn) const
		{
			std::shared_lock lock(rw_);
			if (!overlay_ || !ctl_) {
				return false;
			}
			a_fn(reinterpret_cast<const proto::OverlaySlotHdr*>(ctl_ + proto::kOffOverlaySlotHdr + sizeof(proto::OverlaySlotHdr) * overlayFront_),
				overlay_ + proto::kOverlaySlotBytes * overlayFront_);
			return true;
		}

		// Where the mapping lives, for the log ("file Z:\..." or "section Local\...").
		[[nodiscard]] const char* Where() const { return where_; }

	private:
		Link() = default;
		bool CreateLocked();   // rw_ held exclusively
		void DestroyLocked();  // rw_ held exclusively
		void HeartbeatLoop();
		// The bridge file at kBridgeFileWine is missing or no longer the one we have open.
		bool BridgeFileReplaced() const;

		template <class T>
		T* Ctl(std::uint64_t a_off) const { return reinterpret_cast<T*>(ctl_ + a_off); }

		mutable std::shared_mutex rw_;
		void*                     file_{ nullptr };     // HANDLE (file transport only)
		void*                     mapping_{ nullptr };  // HANDLE
		std::uint8_t*             ctl_{ nullptr };      // [0, kOffOverlayPixels)
		std::uint8_t*             overlay_{ nullptr };  // [kOffOverlayPixels, kOffRenderRing)
		std::uint8_t*             render_{ nullptr };   // the render ring (its view starts at a 64 KiB boundary before it)
		std::uint8_t*             renderView_{ nullptr };
		std::atomic<bool>          ready_{ false };
		std::atomic<bool>          overlayMapped_{ false };
		std::atomic<std::uint32_t> generation_{ 0 };
		std::atomic<bool>          threadStarted_{ false };
		std::mutex                 inputMutex_;
		std::uint32_t              overlayFront_{ 2 };  // render thread only
		std::atomic<bool>          overlayResetPending_{ false };
		char                       where_[160]{};
		std::uint64_t              lastCreateAttemptMs_{ 0 };
		bool                       loggedCreateFailure_{ false };
	};
}

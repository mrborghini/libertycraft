// The OS-independent half of the shared-memory link: seqlocks, the SPSC entry rings, the byte
// rings with pad records and the overlay triple-buffer swap, all on raw pointers into the mapping.
// Link.cpp wraps these around the Windows mapping; tests/link_test.cpp runs them on Linux.
//
// C++17 has no std::atomic_ref, so plain mapped fields are accessed through the GCC/Clang
// __atomic builtins (both clang-cl and host clang have them). On i686 the 64-bit ones compile to
// lock cmpxchg8b / SSE moves; every 64-bit field in the protocol is 8-byte aligned.
#pragma once

#include "libertycraft_protocol.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#if !defined(__clang__) && !defined(__GNUC__)
#	error "LinkCore.h uses the __atomic builtins: build with clang-cl / clang / gcc"
#endif

namespace lc::core
{
	namespace proto = ::libertycraft::proto;

	// ---- atomics on mapped memory ------------------------------------------------------------
	template <class T>
	inline T LoadAcquire(const T* a_p) { return __atomic_load_n(a_p, __ATOMIC_ACQUIRE); }
	template <class T>
	inline T LoadRelaxed(const T* a_p) { return __atomic_load_n(a_p, __ATOMIC_RELAXED); }
	template <class T>
	inline void StoreRelease(T* a_p, T a_v) { __atomic_store_n(a_p, a_v, __ATOMIC_RELEASE); }
	template <class T>
	inline void StoreRelaxed(T* a_p, T a_v) { __atomic_store_n(a_p, a_v, __ATOMIC_RELAXED); }
	template <class T>
	inline T Exchange(T* a_p, T a_v) { return __atomic_exchange_n(a_p, a_v, __ATOMIC_ACQ_REL); }
	inline void FenceRelease() { __atomic_thread_fence(__ATOMIC_RELEASE); }
	inline void FenceAcquire() { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
	inline void CpuRelax()
	{
#if defined(__i386__) || defined(__x86_64__)
		__builtin_ia32_pause();
#endif
	}

	template <class T>
	inline T* At(std::uint8_t* a_base, std::uint64_t a_off) { return reinterpret_cast<T*>(a_base + a_off); }

	// ---- seqlock (first u32 of the block is the sequence; odd while a write is in progress) ----
	// Single writer. Copies a_bytes - 4 bytes after the sequence word.
	inline void SeqlockWrite(void* a_dst, const void* a_src, std::size_t a_bytes)
	{
		auto* seq = static_cast<std::uint32_t*>(a_dst);
		const auto s = LoadRelaxed(seq);
		StoreRelaxed(seq, s + 1);
		FenceRelease();
		std::memcpy(static_cast<std::uint8_t*>(a_dst) + 4, static_cast<const std::uint8_t*>(a_src) + 4, a_bytes - 4);
		StoreRelease(seq, s + 2);
	}

	// Copies a consistent snapshot of a_bytes (including the sequence word) into a_out.
	// Returns false if the writer kept it busy for a_attempts tries.
	inline bool SeqlockRead(const void* a_src, void* a_out, std::size_t a_bytes, int a_attempts = 64)
	{
		const auto* seq = static_cast<const std::uint32_t*>(a_src);
		for (int attempt = 0; attempt < a_attempts; ++attempt) {
			const auto s1 = LoadAcquire(seq);
			if (s1 & 1) {
				CpuRelax();
				continue;
			}
			std::memcpy(a_out, a_src, a_bytes);
			FenceAcquire();
			if (LoadRelaxed(seq) == s1) {
				return true;
			}
		}
		return false;
	}

	// ---- SPSC entry ring: u64 head @ +headOff (producer), u64 tail @ +tailOff (consumer) ------
	struct EntryRing
	{
		std::uint8_t* base;
		std::uint64_t headOff, tailOff, dataOff;
		std::uint32_t entries;  // power of two
		std::uint32_t entryBytes;

		std::uint64_t* Head() const { return reinterpret_cast<std::uint64_t*>(base + headOff); }
		std::uint64_t* Tail() const { return reinterpret_cast<std::uint64_t*>(base + tailOff); }
		std::uint8_t*  Slot(std::uint64_t a_index) const { return base + dataOff + (a_index & (entries - 1)) * entryBytes; }

		// Producer. False (entry dropped) when the consumer is a full ring behind.
		bool Push(const void* a_entry) const
		{
			const auto head = LoadRelaxed(Head());
			const auto tail = LoadAcquire(Tail());
			if (head - tail >= entries) {
				return false;
			}
			std::memcpy(Slot(head), a_entry, entryBytes);
			StoreRelease(Head(), head + 1);
			return true;
		}

		// Consumer. False when empty. If the producer lapped us, skips to the oldest entry it kept.
		bool Pop(void* a_out) const
		{
			const auto head = LoadAcquire(Head());
			auto       tail = LoadRelaxed(Tail());
			if (tail >= head) {
				return false;
			}
			if (head - tail > entries) {
				tail = head - entries;
			}
			std::memcpy(a_out, Slot(tail), entryBytes);
			StoreRelease(Tail(), tail + 1);
			return true;
		}
	};

	inline EntryRing InputRing(std::uint8_t* a_ringBase)
	{
		return { a_ringBase, proto::kInputRingHeadOff, proto::kInputRingTailOff, proto::kInputRingDataOff, proto::kInputRingEntries,
			sizeof(proto::InputEvent) };
	}

	inline EntryRing EventRing(std::uint8_t* a_ringBase)
	{
		return { a_ringBase, proto::kEventRingHeadOff, proto::kEventRingTailOff, proto::kEventRingDataOff, proto::kEventRingEntries,
			sizeof(proto::McEvent) };
	}

	// ---- byte ring (collision ring, render ring): {u64 head @0, u64 tail @0x40, data @0x80} -----
	// Messages are {u32 type, u32 payloadBytes} + payload, padded to 8 bytes, never split: a
	// message that doesn't fit before the end is preceded by a type-0 pad header meaning "skip
	// to the start of the ring". head/tail count total bytes ever written/consumed.
	struct ByteRing
	{
		std::uint8_t* base;
		std::uint64_t dataBytes;

		static constexpr std::uint64_t kHeadOff = 0x00;
		static constexpr std::uint64_t kTailOff = 0x40;
		static constexpr std::uint64_t kDataOff = 0x80;
		static_assert(kHeadOff == proto::kColRingHeadOff && kTailOff == proto::kColRingTailOff && kDataOff == proto::kColRingDataOff);
		static_assert(kHeadOff == proto::kRenRingHeadOff && kTailOff == proto::kRenRingTailOff && kDataOff == proto::kRenRingDataOff);

		std::uint64_t* Head() const { return reinterpret_cast<std::uint64_t*>(base + kHeadOff); }
		std::uint64_t* Tail() const { return reinterpret_cast<std::uint64_t*>(base + kTailOff); }
		std::uint8_t*  Data() const { return base + kDataOff; }

		static std::uint64_t MessageBytes(std::uint32_t a_payload) { return (sizeof(proto::ColMsgHeader) + std::uint64_t(a_payload) + 7) & ~7ull; }

		// Largest payload Write accepts (half the ring, like SkyCraft).
		std::uint64_t MaxPayload() const { return dataBytes / 2 - sizeof(proto::ColMsgHeader); }

		enum class WriteResult { kOk, kFull, kTooLarge };

		// Producer (one thread). kFull: try again later; nothing was written.
		WriteResult Write(std::uint32_t a_type, const void* a_payload, std::uint32_t a_bytes) const
		{
			const std::uint64_t msgBytes = MessageBytes(a_bytes);
			if (msgBytes > dataBytes / 2) {
				return WriteResult::kTooLarge;
			}
			auto       head = LoadRelaxed(Head());
			const auto tail = LoadAcquire(Tail());
			auto       pos = head % dataBytes;
			const auto padBytes = (pos + msgBytes > dataBytes) ? dataBytes - pos : 0;
			if (dataBytes - (head - tail) < msgBytes + padBytes) {
				return WriteResult::kFull;
			}
			if (padBytes) {
				*reinterpret_cast<proto::ColMsgHeader*>(Data() + pos) = { proto::kColPad, 0 };
				head += padBytes;
				pos = 0;
			}
			*reinterpret_cast<proto::ColMsgHeader*>(Data() + pos) = { a_type, a_bytes };
			if (a_bytes) {
				std::memcpy(Data() + pos + sizeof(proto::ColMsgHeader), a_payload, a_bytes);
			}
			StoreRelease(Head(), head + msgBytes);
			return WriteResult::kOk;
		}

		// Bytes written but not yet consumed.
		std::uint64_t Pending() const { return LoadAcquire(Head()) - LoadRelaxed(Tail()); }

		// Consumer (one thread): a_fn(type, payload, payloadBytes) for each pending message until
		// about a_maxBytes have been consumed. The payload points into the ring. Returns the bytes
		// consumed (pads included). A corrupt header (payload running past the written data)
		// resynchronises by dropping everything pending.
		template <class Fn>
		std::uint64_t Drain(Fn&& a_fn, std::uint64_t a_maxBytes) const
		{
			const auto    head = LoadAcquire(Head());
			auto          tail = LoadRelaxed(Tail());
			std::uint64_t done = 0;
			while (tail < head && done < a_maxBytes) {
				const auto  pos = tail % dataBytes;
				const auto* hdr = reinterpret_cast<const proto::ColMsgHeader*>(Data() + pos);
				if (hdr->type == proto::kColPad) {
					done += dataBytes - pos;
					tail += dataBytes - pos;
					continue;
				}
				const auto msgBytes = MessageBytes(hdr->payloadBytes);
				if (pos + msgBytes > dataBytes || tail + msgBytes > head) {
					done += head - tail;
					tail = head;
					break;
				}
				a_fn(hdr->type, Data() + pos + sizeof(proto::ColMsgHeader), hdr->payloadBytes);
				tail += msgBytes;
				done += msgBytes;
			}
			StoreRelease(Tail(), tail);
			return done;
		}
	};

	// ---- overlay triple buffer (consumer side) --------------------------------------------------
	// state bits 0-1: the middle slot, bit 2: it holds an unread frame. If a newer frame is there,
	// swaps our front slot in for it and returns true (a_front then names the new front slot).
	inline bool OverlayAcquire(std::uint32_t* a_state, std::uint32_t& a_front)
	{
		if (!(LoadAcquire(a_state) & proto::kOverlayDirty)) {
			return false;
		}
		const auto old = Exchange(a_state, a_front);
		a_front = old & 3;
		return true;
	}

	// ---- the host's share of the mapping, reset when the host attaches -------------------------
	// a_ctl is the mapping from offset 0 up to kOffOverlayPixels; a_render the render ring.
	inline void ResetHostRegions(std::uint8_t* a_ctl, std::uint8_t* a_render, std::uint32_t a_pid, std::uint64_t a_nowMs)
	{
		auto* header = At<proto::Header>(a_ctl, proto::kOffHeader);
		std::memset(a_ctl + proto::kOffSkyState, 0, sizeof(proto::SkyState));
		std::memset(a_ctl + proto::kOffOverlayCtl, 0, 0x100);
		std::memset(a_ctl + proto::kOffWaterGrid, 0, sizeof(proto::WaterGrid));
		std::memset(a_ctl + proto::kOffInputRing, 0, proto::kInputRingDataOff);
		std::memset(a_ctl + proto::kOffCollisionRing, 0, proto::kColRingDataOff);
		std::memset(a_ctl + proto::kOffActorTable, 0, sizeof(proto::ActorTable));
		std::memset(a_ctl + proto::kOffEventRing, 0, proto::kEventRingDataOff);
		std::memset(a_ctl + proto::kOffWorldEntities, 0, sizeof(proto::WorldEntities));
		if (a_render) {
			std::memset(a_render, 0, proto::kRenRingDataOff);
		}
		// Minecraft tells a new host instance by skyrimPid changing OR mcPid cleared (a fresh
		// wineserver can hand out the same pid again), so clear it before publishing.
		StoreRelease(&header->mcPid, 0u);
		header->version = proto::kVersion;
		header->skyrimPid = a_pid;
		StoreRelease(&header->skyrimHeartbeatMs, a_nowMs);
		StoreRelease(&header->magic, proto::kMagic);
	}
}

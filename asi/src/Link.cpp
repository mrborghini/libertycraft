#define LC_MODULE "link"
#include "Link.h"

#include "Log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace lc
{
	namespace
	{
		constexpr std::uint64_t kMcTimeoutMs = 3000;
		constexpr std::uint64_t kRetryMs = 2000;
		constexpr std::uint64_t kHeartbeatMs = 250;
		constexpr std::uint64_t kFileCheckMs = 1000;
		constexpr std::uint64_t kGranularity = 0x10000;  // MapViewOfFile offsets: 64 KiB

		static_assert(proto::kOffOverlayPixels % kGranularity == 0);
		constexpr std::uint64_t kRenderViewOff = proto::kOffRenderRing & ~(kGranularity - 1);

		std::uint8_t* MapView(HANDLE a_mapping, std::uint64_t a_off, std::uint64_t a_bytes)
		{
			return static_cast<std::uint8_t*>(::MapViewOfFile(a_mapping, FILE_MAP_ALL_ACCESS, static_cast<DWORD>(a_off >> 32),
				static_cast<DWORD>(a_off & 0xFFFFFFFFu), static_cast<SIZE_T>(a_bytes)));
		}

		bool DirectoryExists(const wchar_t* a_path)
		{
			const DWORD attr = ::GetFileAttributesW(a_path);
			return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
		}
	}

	Link& Link::Get()
	{
		static Link link;
		return link;
	}

	bool Link::Create()
	{
		if (Valid()) {
			return true;
		}
		std::unique_lock lock(rw_);
		return CreateLocked();
	}

	bool Link::CreateLocked()
	{
		if (Valid()) {
			return true;
		}
		const auto now = ::GetTickCount64();
		if (lastCreateAttemptMs_ && now - lastCreateAttemptMs_ < kRetryMs) {
			return false;
		}
		lastCreateAttemptMs_ = now;
		const bool              quiet = loggedCreateFailure_;  // log the first failure of a streak only
		constexpr std::uint64_t size = proto::kMappingBytes;
		const DWORD             sizeHi = static_cast<DWORD>(size >> 32), sizeLo = static_cast<DWORD>(size & 0xFFFFFFFFu);
		auto fail = [&](const char* a_what, DWORD a_error) {
			if (!quiet) {
				LC_LOG("ERROR: %s failed (error %lu); retrying every 2 s", a_what, a_error);
			}
			loggedCreateFailure_ = true;
			DestroyLocked();
			return false;
		};

		const bool useFile = DirectoryExists(L"Z:\\dev\\shm");
		bool       existed = false;
		long long  grewFrom = -1;
		if (useFile) {
			HANDLE file = ::CreateFileW(proto::kBridgeFileWine, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return fail("CreateFileW(Z:\\dev\\shm\\libertycraft-bridge)", ::GetLastError());
			}
			existed = ::GetLastError() == ERROR_ALREADY_EXISTS;
			file_ = file;
			// Only ever grow it: Minecraft may have it mapped, and a shorter file means SIGBUS there.
			LARGE_INTEGER current{};
			if (::GetFileSizeEx(file, &current) && static_cast<std::uint64_t>(current.QuadPart) < size) {
				grewFrom = current.QuadPart;
				LARGE_INTEGER target{};
				target.QuadPart = static_cast<LONGLONG>(size);
				if (!::SetFilePointerEx(file, target, nullptr, FILE_BEGIN) || !::SetEndOfFile(file)) {
					return fail("growing the bridge file", ::GetLastError());
				}
			}
			mapping_ = ::CreateFileMappingW(file, nullptr, PAGE_READWRITE, sizeHi, sizeLo, nullptr);
			if (!mapping_) {
				return fail("CreateFileMappingW(bridge file)", ::GetLastError());
			}
			std::snprintf(where_, sizeof(where_), "file Z:\\dev\\shm\\libertycraft-bridge");
		} else {
			mapping_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, sizeHi, sizeLo, proto::kMappingName);
			if (!mapping_) {
				return fail("CreateFileMappingW(Local\\LibertyCraft_v1)", ::GetLastError());
			}
			existed = ::GetLastError() == ERROR_ALREADY_EXISTS;
			std::snprintf(where_, sizeof(where_), "section Local\\LibertyCraft_v1 (no Z:\\dev\\shm: native Windows?)");
		}

		ctl_ = MapView(mapping_, 0, proto::kOffOverlayPixels);
		if (!ctl_) {
			return fail("MapViewOfFile(control view, 32 MiB)", ::GetLastError());
		}
		renderView_ = MapView(mapping_, kRenderViewOff, size - kRenderViewOff);
		if (!renderView_) {
			return fail("MapViewOfFile(render view, 64 MiB)", ::GetLastError());
		}
		render_ = renderView_ + (proto::kOffRenderRing - kRenderViewOff);
		overlay_ = MapView(mapping_, proto::kOffOverlayPixels, proto::kOverlaySlotBytes * proto::kOverlaySlots);
		if (!overlay_) {
			LC_LOG("WARNING: couldn't map the overlay view (%llu MiB, error %lu): the Minecraft overlay stays off",
				static_cast<unsigned long long>((proto::kOverlaySlotBytes * proto::kOverlaySlots) >> 20), ::GetLastError());
		}

		const auto* header = Ctl<proto::Header>(proto::kOffHeader);
		const auto  oldMcPid = header->mcPid;
		const auto  oldMcBeat = header->mcHeartbeatMs;
		// A stale mapping survives when Minecraft kept it open across a game restart: reset all the
		// host owns so rings and the overlay swap start from a known state, then publish the magic.
		core::ResetHostRegions(ctl_, render_, ::GetCurrentProcessId(), ::GetTickCount64());
		overlayResetPending_.store(true, std::memory_order_release);
		overlayMapped_.store(overlay_ != nullptr, std::memory_order_release);
		loggedCreateFailure_ = false;
		const auto gen = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
		ready_.store(true, std::memory_order_release);

		char grew[64] = "";
		if (grewFrom >= 0) {
			std::snprintf(grew, sizeof(grew), ", grown from %lld bytes", grewFrom);
		}
		LC_LOG("link created (#%u): %s, %llu bytes (%llu MiB), %s%s; views ctl=%p overlay=%p render=%p; protocol v%u magic 0x%08X pid %lu", gen, where_,
			static_cast<unsigned long long>(size), static_cast<unsigned long long>(size >> 20), existed ? "existing" : "new", grew,
			static_cast<void*>(ctl_), static_cast<void*>(overlay_), static_cast<void*>(render_), proto::kVersion, proto::kMagic, ::GetCurrentProcessId());
		if (oldMcPid || oldMcBeat) {
			LC_LOG("link: a Minecraft was attached before us (pid %u, heartbeat stamp %llu, now %llu); mcPid cleared", oldMcPid,
				static_cast<unsigned long long>(oldMcBeat), static_cast<unsigned long long>(::GetTickCount64()));
		}
		return true;
	}

	void Link::Destroy()
	{
		std::unique_lock lock(rw_);
		DestroyLocked();
	}

	void Link::DestroyLocked()
	{
		ready_.store(false, std::memory_order_release);
		overlayMapped_.store(false, std::memory_order_release);
		if (overlay_) {
			::UnmapViewOfFile(overlay_);
		}
		if (renderView_) {
			::UnmapViewOfFile(renderView_);
		}
		if (ctl_) {
			::UnmapViewOfFile(ctl_);
		}
		overlay_ = renderView_ = render_ = ctl_ = nullptr;
		if (mapping_) {
			::CloseHandle(static_cast<HANDLE>(mapping_));
			mapping_ = nullptr;
		}
		if (file_) {
			::CloseHandle(static_cast<HANDLE>(file_));
			file_ = nullptr;
		}
	}

	bool Link::BridgeFileReplaced() const
	{
		if (!file_) {
			return false;  // named section: nothing to watch
		}
		HANDLE fresh = ::CreateFileW(proto::kBridgeFileWine, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (fresh == INVALID_HANDLE_VALUE) {
			const DWORD error = ::GetLastError();
			return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
		}
		BY_HANDLE_FILE_INFORMATION a{}, b{};
		const bool ok = ::GetFileInformationByHandle(static_cast<HANDLE>(file_), &a) && ::GetFileInformationByHandle(fresh, &b);
		::CloseHandle(fresh);
		return ok && (a.dwVolumeSerialNumber != b.dwVolumeSerialNumber || a.nFileIndexHigh != b.nFileIndexHigh || a.nFileIndexLow != b.nFileIndexLow);
	}

	void Link::StartHeartbeatThread()
	{
		if (threadStarted_.exchange(true)) {
			return;
		}
		// CreateThread, not std::thread: this may run inside DllMain (loader lock), where the new
		// thread must not be waited for; it starts once DllMain returns.
		HANDLE thread = ::CreateThread(
			nullptr, 0, [](LPVOID a_self) -> DWORD {
				static_cast<Link*>(a_self)->HeartbeatLoop();
				return 0;
			},
			this, 0, nullptr);
		if (thread) {
			::CloseHandle(thread);
		} else {
			threadStarted_ = false;
			LC_LOG("ERROR: couldn't start the heartbeat thread (error %lu)", ::GetLastError());
		}
	}

	void Link::HeartbeatLoop()
	{
		std::uint64_t lastFileCheck = 0;
		for (;;) {
			::Sleep(static_cast<DWORD>(kHeartbeatMs));
			if (!Valid()) {
				Create();
				continue;
			}
			Heartbeat();
			// Minecraft deletes the bridge file when it exits and the host looks dead. While its
			// heartbeat is missing, make sure we still hold the file a restarted Minecraft opens.
			const auto now = ::GetTickCount64();
			if (McHeartbeatAgeMs() > kMcTimeoutMs && now - lastFileCheck >= kFileCheckMs) {
				lastFileCheck = now;
				std::unique_lock lock(rw_);
				if (Valid() && BridgeFileReplaced()) {
					LC_LOG("link: the bridge file was deleted or replaced (Minecraft exited?); reopening it");
					DestroyLocked();
					lastCreateAttemptMs_ = 0;
					CreateLocked();
				}
			}
		}
	}

	void Link::Heartbeat()
	{
		std::shared_lock lock(rw_);
		if (ctl_) {
			core::StoreRelease(&Ctl<proto::Header>(proto::kOffHeader)->skyrimHeartbeatMs, ::GetTickCount64());
		}
	}

	std::uint64_t Link::McHeartbeatAgeMs() const
	{
		std::shared_lock lock(rw_);
		if (!ctl_) {
			return UINT64_MAX;
		}
		const auto last = core::LoadAcquire(&Ctl<proto::Header>(proto::kOffHeader)->mcHeartbeatMs);
		if (last == 0) {
			return UINT64_MAX;
		}
		const auto now = ::GetTickCount64();
		return now > last ? now - last : 0;
	}

	bool Link::MinecraftAlive() const
	{
		return McHeartbeatAgeMs() < kMcTimeoutMs;
	}

	std::uint32_t Link::McPid() const
	{
		std::shared_lock lock(rw_);
		return ctl_ ? core::LoadAcquire(&Ctl<proto::Header>(proto::kOffHeader)->mcPid) : 0;
	}

	void Link::WriteSkyState(const proto::SkyState& a_state)
	{
		std::shared_lock lock(rw_);
		if (ctl_) {
			core::SeqlockWrite(Ctl<proto::SkyState>(proto::kOffSkyState), &a_state, sizeof(a_state));
		}
	}

	void Link::WriteWaterGrid(const proto::WaterGrid& a_grid)
	{
		std::shared_lock lock(rw_);
		if (ctl_) {
			core::SeqlockWrite(Ctl<proto::WaterGrid>(proto::kOffWaterGrid), &a_grid, sizeof(a_grid));
		}
	}

	void Link::WriteActors(const proto::ActorRecord* a_records, std::uint32_t a_count)
	{
		std::shared_lock lock(rw_);
		if (!ctl_) {
			return;
		}
		auto*      table = Ctl<proto::ActorTable>(proto::kOffActorTable);
		const auto s = core::LoadRelaxed(&table->seq);
		core::StoreRelaxed(&table->seq, s + 1);
		core::FenceRelease();
		const auto count = a_count < proto::kMaxActors ? a_count : proto::kMaxActors;
		table->count = count;
		{
			// When this table was written (QPC in 100 ns units, as McState::tickQpc): Minecraft takes the
			// vehicles' speeds from it. Wine's counter runs at 10 MHz already.
			LARGE_INTEGER q{}, f{};
			::QueryPerformanceCounter(&q);
			::QueryPerformanceFrequency(&f);
			const auto ticks = static_cast<std::uint64_t>(q.QuadPart), freq = static_cast<std::uint64_t>(f.QuadPart);
			table->stamp = freq == 10'000'000u || freq == 0 ? ticks : ticks / freq * 10'000'000u + ticks % freq * 10'000'000u / freq;
		}
		if (count) {
			std::memcpy(table->actors, a_records, sizeof(proto::ActorRecord) * count);
		}
		core::StoreRelease(&table->seq, s + 2);
	}

	bool Link::ReadMcState(proto::McState& a_out) const
	{
		std::shared_lock lock(rw_);
		return ctl_ && core::SeqlockRead(Ctl<proto::McState>(proto::kOffMcState), &a_out, sizeof(a_out));
	}

	bool Link::ReadWorldEntities(proto::WorldEntities& a_out) const
	{
		std::shared_lock lock(rw_);
		if (!ctl_) {
			return false;
		}
		const auto* src = Ctl<proto::WorldEntities>(proto::kOffWorldEntities);
		for (int attempt = 0; attempt < 16; ++attempt) {
			const auto s1 = core::LoadAcquire(&src->seq);
			if (s1 & 1) {
				core::CpuRelax();
				continue;
			}
			auto count = src->count;
			count = count < proto::kMaxWorldEntities ? count : proto::kMaxWorldEntities;
			std::memcpy(&a_out, src, offsetof(proto::WorldEntities, entities) + sizeof(proto::WorldEntity) * count);
			a_out.count = count;
			core::FenceAcquire();
			if (core::LoadRelaxed(&src->seq) == s1) {
				return true;
			}
		}
		return false;
	}

	bool Link::PushInput(proto::InputType a_type, std::uint16_t a_code, std::int32_t a_a, std::int32_t a_b, std::int32_t a_c)
	{
		const proto::InputEvent event{ static_cast<std::uint16_t>(a_type), a_code, a_a, a_b, a_c };
		std::shared_lock lock(rw_);
		if (!ctl_) {
			return false;
		}
		std::lock_guard producer(inputMutex_);
		return core::InputRing(ctl_ + proto::kOffInputRing).Push(&event);
	}

	bool Link::PopEvent(proto::McEvent& a_out)
	{
		std::shared_lock lock(rw_);
		return ctl_ && core::EventRing(ctl_ + proto::kOffEventRing).Pop(&a_out);
	}

	bool Link::WriteCollision(proto::ColType a_type, const void* a_payload, std::uint32_t a_bytes)
	{
		std::shared_lock lock(rw_);
		if (!ctl_) {
			return false;
		}
		const auto result = core::ByteRing{ ctl_ + proto::kOffCollisionRing, proto::kColRingDataBytes }.Write(a_type, a_payload, a_bytes);
		if (result == core::ByteRing::WriteResult::kTooLarge) {
			LC_LOG("ERROR: collision message type %u too large (%u bytes); dropped", static_cast<unsigned>(a_type), a_bytes);
		}
		return result == core::ByteRing::WriteResult::kOk;
	}

	std::uint64_t Link::CollisionPending() const
	{
		std::shared_lock lock(rw_);
		return ctl_ ? core::ByteRing{ ctl_ + proto::kOffCollisionRing, proto::kColRingDataBytes }.Pending() : 0;
	}

	bool Link::AcquireOverlayFrame()
	{
		std::shared_lock lock(rw_);
		if (!ctl_ || !overlay_) {
			return false;
		}
		if (overlayResetPending_.exchange(false, std::memory_order_acq_rel)) {
			overlayFront_ = 2;
		}
		return core::OverlayAcquire(&Ctl<proto::OverlayCtl>(proto::kOffOverlayCtl)->state, overlayFront_);
	}

	void Link::ResetOverlay()
	{
		std::shared_lock lock(rw_);
		if (!ctl_) {
			return;
		}
		core::StoreRelease(&Ctl<proto::OverlayCtl>(proto::kOffOverlayCtl)->state, 0u);
		overlayResetPending_.store(true, std::memory_order_release);  // overlayFront_ is the render thread's
	}
}

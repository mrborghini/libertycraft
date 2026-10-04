// Unity-built into dllmain.cpp (needs IV-SDK). See BlockyCity.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "city"
#include "BlockyCity.h"

#include "Log.h"
#include "Perf.h"

#include <cstdint>
#include <unordered_map>

namespace lc::BlockyCity
{
	namespace
	{
		constexpr std::uint32_t kCastShadows = 0x80;  // CEntity::m_nEntityFlags2

		struct Saved
		{
			std::int16_t  model;
			std::uint32_t flags2;
		};

		bool                                  active = false;
		std::unordered_map<CEntity*, Saved>   hidden;  // what we hid (to put back), game thread only
		float                                 logT = 0.0f;
		std::uint32_t                         scans = 0;
		double                                scanMsSum = 0.0, scanMsMax = 0.0;

		// The 32-bit game's address space in use (MiB): what the blocks' managed buffers eat into.
		std::uint64_t AddressSpaceUsedMiB(std::uint64_t* a_totalMiB)
		{
			MEMORYSTATUSEX ms{};
			ms.dwLength = sizeof(ms);
			if (!::GlobalMemoryStatusEx(&ms)) {
				*a_totalMiB = 0;
				return 0;
			}
			*a_totalMiB = ms.ullTotalVirtual >> 20;
			return (ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20;
		}

		// A loose object that belongs to the map (street furniture, props, doors), not to someone.
		bool MapObject(CObject* a_obj)
		{
			if (a_obj->m_pAttachedToEntity) {
				return false;  // held by a ped, hanging off a car
			}
			const int model = a_obj->m_nModelIndex;
			CBaseModelInfo* mi = model >= 0 && model < 31000 ? CModelInfo::ms_modelInfoPtrs[model] : nullptr;
			if (!mi) {
				return false;
			}
			const auto type = mi->GetModelType();
			return type != MI_TYPE_WEAPON && type != MI_TYPE_VEHICLE && type != MI_TYPE_PED;
		}

		void Hide(CEntity* a_e)
		{
			if (!a_e->m_nEntityFlags.bIsVisible) {
				return;
			}
			hidden[a_e] = Saved{ a_e->m_nModelIndex, a_e->m_nEntityFlags2 };
			a_e->m_nEntityFlags.bIsVisible = 0;
			a_e->m_nEntityFlags2 &= ~kCastShadows;
		}

		// Hides what's visible of the map now (what streamed in since the last frame too).
		void HideAll(std::uint32_t& a_buildings, std::uint32_t& a_objects)
		{
			if (auto* pool = CPools::ms_pBuildingPool) {
				for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
					if (CBuilding* b = pool->Get(slot)) {
						Hide(b);
						++a_buildings;
					}
				}
			}
			if (auto* pool = CPools::ms_pObjectPool) {
				for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
					CObject* o = pool->Get(slot);
					if (o && MapObject(o)) {
						Hide(o);
						++a_objects;
					}
				}
			}
		}

		// Puts back what we hid and is still there (the same entity: same slot contents, same model).
		std::uint32_t ShowAll()
		{
			std::uint32_t shown = 0;
			auto restore = [&](CEntity* a_e) {
				auto it = hidden.find(a_e);
				if (it == hidden.end() || it->second.model != a_e->m_nModelIndex) {
					return;
				}
				a_e->m_nEntityFlags.bIsVisible = 1;
				a_e->m_nEntityFlags2 = (a_e->m_nEntityFlags2 & ~kCastShadows) | (it->second.flags2 & kCastShadows);
				++shown;
			};
			if (auto* pool = CPools::ms_pBuildingPool) {
				for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
					if (CBuilding* b = pool->Get(slot)) {
						restore(b);
					}
				}
			}
			if (auto* pool = CPools::ms_pObjectPool) {
				for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
					if (CObject* o = pool->Get(slot)) {
						restore(o);
					}
				}
			}
			const auto count = hidden.size();
			hidden.clear();
			LC_LOG("blocky city: GTA's map shown again (%u of %zu hidden entities still there)", shown, count);
			return shown;
		}
	}

	void Tick(bool a_inCity, float a_dt)
	{
		if (a_inCity && !active) {
			active = true;
			logT = 0.0f;
			scans = 0;
			scanMsSum = scanMsMax = 0.0;
			auto* bp = CPools::ms_pBuildingPool;
			auto* op = CPools::ms_pObjectPool;
			LC_LOG("blocky city: Minecraft's player went through a portal (kMcBlockyCity): hiding GTA's map geometry (building pool %u slots, object pool %u)",
				bp ? bp->m_nCount : 0u, op ? op->m_nCount : 0u);
		} else if (!a_inCity && active) {
			active = false;
			ShowAll();
			LC_LOG("blocky city: Minecraft's player is back in the mirror world (%u scans, %.3f ms average, %.3f ms worst)", scans,
				scans ? scanMsSum / scans : 0.0, scanMsMax);
			return;
		}
		if (!active) {
			return;
		}
		const auto    t0 = Perf::Now();
		std::uint32_t buildings = 0, objects = 0;
		const auto    before = hidden.size();
		HideAll(buildings, objects);
		const double ms = double(Perf::Now() - t0) / Perf::TicksPerMs();
		++scans;
		scanMsSum += ms;
		scanMsMax = ms > scanMsMax ? ms : scanMsMax;
		if (scans == 1 || (logT -= a_dt) <= 0.0f) {
			logT = 10.0f;
			std::uint64_t total = 0;
			const auto    used = AddressSpaceUsedMiB(&total);
			LC_LOG("blocky city: GTA's map hidden: %u buildings and %u map objects in the pools, %zu hidden so far (+%zu this frame), scan %.3f ms; "
				   "address space %llu of %llu MiB in use",
				buildings, objects, hidden.size(), hidden.size() - before, ms, static_cast<unsigned long long>(used), static_cast<unsigned long long>(total));
		}
	}

	void OnIngameStartup()
	{
		if (active || !hidden.empty()) {
			LC_LOG("blocky city: the game is loading; forgetting %zu hidden entities", hidden.size());
		}
		hidden.clear();
		active = false;
	}

	bool Active()
	{
		return active;
	}
}

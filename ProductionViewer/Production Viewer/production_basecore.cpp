#include "production_basecore.h"
#include "Plugin Core/Helpers/plugin_helpers.h"
#include "Plugin Core/Signatures/plugin_signatures.h"

#include "Basic.hpp"
#include "CoreUObject_classes.hpp"
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "ChimeraMassCommon_structs.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace ProductionBaseCore
{
	namespace
	{
		// TArray<FCrGridEntityHandle, TSizedDefaultAllocator<32>> - 32-bit
		// Num/Max, 16 bytes total. Must be zero-initialized before being passed
		// to GetBaseCoresRangedBuilding (the callee appends without checking).
		struct FGridHandleArray
		{
			void*   Data;
			int32_t ArrayNum;
			int32_t ArrayMax;
		};
		static_assert(sizeof(FGridHandleArray) == 16);

		// FCrGridEntityHandle (24 bytes): { FMassEntityHandle EntityHandle;
		// TSharedPtr<FCrGridEntityDataBase> Data; }. Only the leading entity
		// handle matters to us; Data points at grid-internal cell bookkeeping.
		struct FCrGridEntityHandleRaw
		{
			int32_t Index;
			int32_t SerialNumber;
			void*   DataObject;    // FCrGridEntityDataBase* (unused)
			void*   RefController; // TSharedPtr intrusive refcount controller
		};
		static_assert(sizeof(FCrGridEntityHandleRaw) == 24);

		using GetBaseCoresRangedBuildingFn = void(__fastcall*)(
			SDK::UCrBaseCoreSubsystem* self, ProductionMass::FMassEntityHandle building,
			FGridHandleArray* outBaseCores, bool functionalBaseCoresOnly);

		// Neither of these is a UFUNCTION, so the generated SDK has no callable
		// wrapper - resolved by pattern scan and called directly (same approach
		// as BetterMap's basecore_rename.h, where these patterns come from).
		using ReplicationHelperCtorFn = void(__fastcall*)(
			SDK::FCrMassEntityReplicationHelper* self, ProductionMass::FMassEntityHandle entity,
			const SDK::UObject* worldContextObject);
		using GetNameByReplicationHelperFn = SDK::FString*(__fastcall*)(
			SDK::UCrBuildingCustomNameSubsystem* self, SDK::FString* result,
			SDK::FCrMassEntityReplicationHelper* entity, bool* outIsDefaultName);

		GetBaseCoresRangedBuildingFn g_getBaseCoresRangedBuilding = nullptr;
		ReplicationHelperCtorFn      g_replicationHelperCtor      = nullptr;
		GetNameByReplicationHelperFn g_getNameByReplicationHelper = nullptr;

		constexpr const char* kUnnamedBase = "Unnamed Base";

		// Crafters are stationary, but bases get built/deconstructed/renamed,
		// so cached results are re-queried after a while rather than kept for
		// the whole session.
		constexpr std::chrono::seconds kBaseCacheTtl{60};
		constexpr float kPlayerRefreshIntervalSeconds = 1.0f;

		struct CachedBases
		{
			std::vector<BaseCoreInfo> bases;
			std::chrono::steady_clock::time_point resolvedAt;
		};

		std::mutex g_mutex;
		std::unordered_map<uint64_t, CachedBases> g_buildingBases;  // key: packed building handle
		std::unordered_map<uint64_t, SDK::FVector> g_baseLocations; // key: packed base core handle
		SDK::FVector g_playerLocation{};
		bool  g_playerLocationValid = false;
		float g_playerRefreshTimer  = 0.0f;

		// Releases one strong reference on a returned FCrGridEntityHandle's
		// TSharedPtr controller, mirroring the exact epilogue the game's own
		// consumers of GetBaseCoresRangedBuilding use (e.g.
		// ACrBuildingActorBase::IsBaseCoreOverloaded): strong count at +0x08,
		// weak count at +0x0C, vtable[0](controller) destroys the referenced
		// object, vtable[1](controller, 1) destroys the controller itself.
		// The array we receive holds the sole owning reference per element, so
		// skipping this would leak the referenced FCrGridEntityDataBase.
		void ReleaseGridHandleRef(void* refController)
		{
			if (!refController)
				return;

			auto* base = static_cast<uint8_t*>(refController);
			auto* strongCount = reinterpret_cast<volatile LONG*>(base + 0x8);
			if (InterlockedDecrement(strongCount) == 0)
			{
				void** vtable = *reinterpret_cast<void***>(refController);
				reinterpret_cast<void(__fastcall*)(void*)>(vtable[0])(refController);

				auto* weakCount = reinterpret_cast<volatile LONG*>(base + 0xC);
				if (InterlockedDecrement(weakCount) == 0)
					reinterpret_cast<void(__fastcall*)(void*, int64_t)>(vtable[1])(refController, 1);
			}
		}

		// Resolves a base core's player-assigned custom name from its entity
		// handle via UCrBuildingCustomNameSubsystem - no AActor involved.
		// Adapted from BetterMap::BaseCoreRename::GetBaseCoreCustomName.
		std::string ResolveBaseCoreName(SDK::UWorld* world, ProductionMass::FMassEntityHandle handle)
		{
			if (!world || !g_replicationHelperCtor || !g_getNameByReplicationHelper)
			{
				LOG_TRACE("ProductionBaseCore: ResolveBaseCoreName - name resolution unavailable "
					"(world=%p ctor=%p getName=%p)", static_cast<void*>(world),
					reinterpret_cast<void*>(g_replicationHelperCtor),
					reinterpret_cast<void*>(g_getNameByReplicationHelper));
				return kUnnamedBase;
			}

			auto* nameSys = static_cast<SDK::UCrBuildingCustomNameSubsystem*>(
				SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(world, SDK::UCrBuildingCustomNameSubsystem::StaticClass()));
			if (!nameSys)
			{
				LOG_DEBUG("ProductionBaseCore: no UCrBuildingCustomNameSubsystem - using default name");
				return kUnnamedBase;
			}

			SDK::FCrMassEntityReplicationHelper helper{};
			g_replicationHelperCtor(&helper, handle, static_cast<const SDK::UObject*>(world));
			LOG_TRACE("ProductionBaseCore: ResolveBaseCoreName - handle Index=%u Serial=%u -> "
				"helper NetID=%u PersistentID=%u", handle.Index, handle.SerialNumber,
				*reinterpret_cast<const uint32_t*>(&helper.NetID), helper.Entity.ID);

			// On a multiplayer client, a default-name result also means the
			// subsystem just kicked off an async server request - the real name
			// lands in its cache for the next TTL refresh.
			bool isDefaultName = true;
			SDK::FString result{};
			g_getNameByReplicationHelper(nameSys, &result, &helper, &isDefaultName);

			std::string name = result.ToString();
			LOG_DEBUG("ProductionBaseCore: ResolveBaseCoreName - Index=%u Serial=%u -> '%s' (isDefaultName=%d)",
				handle.Index, handle.SerialNumber, name.c_str(), isDefaultName ? 1 : 0);

			if (name.empty() || isDefaultName)
				return kUnnamedBase;

			return name;
		}

		// Runs the game's containment query for `building` and resolves each
		// returned base core's name + current location. Handles/locations are
		// only trusted here, immediately after the game's own query returned
		// them - never from stale cached handles.
		std::vector<BaseCoreInfo> QueryBaseCores(SDK::UWorld* world, ProductionMass::FMassEntityHandle building)
		{
			std::vector<BaseCoreInfo> result;

			auto* subsystem = static_cast<SDK::UCrBaseCoreSubsystem*>(
				SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(world, SDK::UCrBaseCoreSubsystem::StaticClass()));
			if (!subsystem)
			{
				LOG_DEBUG("ProductionBaseCore: no UCrBaseCoreSubsystem - reporting no base cores");
				return result;
			}

			LOG_TRACE("ProductionBaseCore: QueryBaseCores - subsystem=%p building Index=%u Serial=%u",
				static_cast<void*>(subsystem), building.Index, building.SerialNumber);

			FGridHandleArray out{};
			g_getBaseCoresRangedBuilding(subsystem, building, &out, /*functionalBaseCoresOnly*/ false);

			LOG_DEBUG("ProductionBaseCore: query for building Index=%u Serial=%u returned %d base core(s) "
				"(data=%p max=%d)", building.Index, building.SerialNumber, out.ArrayNum, out.Data, out.ArrayMax);

			auto* elements = static_cast<FCrGridEntityHandleRaw*>(out.Data);
			for (int32_t i = 0; i < out.ArrayNum; ++i)
			{
				BaseCoreInfo info;
				info.handle.Index = static_cast<uint32_t>(elements[i].Index);
				info.handle.SerialNumber = static_cast<uint32_t>(elements[i].SerialNumber);
				info.name = ResolveBaseCoreName(world, info.handle);

				LOG_DEBUG("ProductionBaseCore: base core Index=%u Serial=%u name='%s'",
					info.handle.Index, info.handle.SerialNumber, info.name.c_str());

				result.push_back(std::move(info));
				ReleaseGridHandleRef(elements[i].RefController);
			}

			// The backing buffer was allocated by the engine's FMemory - free
			// it through the modloader's allocator bridge, never the CRT.
			if (out.Data)
			{
				IPluginHooks* hooks = GetHooks();
				if (hooks && hooks->Memory && hooks->Memory->IsAllocatorAvailable())
					hooks->Memory->Free(out.Data);
				else
					LOG_WARN("ProductionBaseCore: engine allocator unavailable - leaking %d-element query buffer", out.ArrayNum);
			}

			// Refresh each base core's cached world location while its handle
			// is known-fresh.
			for (const BaseCoreInfo& info : result)
			{
				SDK::FVector location{};
				if (ProductionMass::GetEntityLocation(world, info.handle, location))
				{
					LOG_TRACE("ProductionBaseCore: base core Index=%u Serial=%u location=(%.0f, %.0f, %.0f)",
						info.handle.Index, info.handle.SerialNumber, location.X, location.Y, location.Z);
					std::lock_guard<std::mutex> lock(g_mutex);
					g_baseLocations[PackHandle(info.handle)] = location;
				}
				else
				{
					LOG_DEBUG("ProductionBaseCore: failed to resolve location for base core Index=%u Serial=%u "
						"- distance will show as unknown", info.handle.Index, info.handle.SerialNumber);
				}
			}

			return result;
		}
	}

	bool Init(IPluginSelf* self)
	{
		(void)self;

		IPluginScanner* scanner = GetScanner();
		if (!scanner)
		{
			LOG_WARN("ProductionBaseCore: scanner unavailable - items will report 'Unknown Location'");
			return false;
		}

		uintptr_t queryAddr = scanner->FindPatternInMainModule(Signatures::GetBaseCoresRangedBuilding);
		if (!queryAddr)
		{
			LOG_WARN("ProductionBaseCore: UCrBaseCoreSubsystem::GetBaseCoresRangedBuilding pattern not found - "
				"items will report 'Unknown Location'");
			return false;
		}
		g_getBaseCoresRangedBuilding = reinterpret_cast<GetBaseCoresRangedBuildingFn>(queryAddr);
		LOG_DEBUG("ProductionBaseCore: GetBaseCoresRangedBuilding pattern matched at 0x%llX",
			static_cast<unsigned long long>(queryAddr));

		uintptr_t ctorAddr = scanner->FindPatternInMainModule(Signatures::BaseCoreReplicationHelperCtor);
		if (ctorAddr)
		{
			g_replicationHelperCtor = reinterpret_cast<ReplicationHelperCtorFn>(ctorAddr);
			LOG_DEBUG("ProductionBaseCore: FCrMassEntityReplicationHelper ctor pattern matched at 0x%llX",
				static_cast<unsigned long long>(ctorAddr));
		}
		else
			LOG_WARN("ProductionBaseCore: FCrMassEntityReplicationHelper ctor pattern not found - "
				"base cores will show as '%s'", kUnnamedBase);

		uintptr_t nameAddr = scanner->FindPatternInMainModule(Signatures::GetBuildingCustomNameByReplicationHelper);
		if (nameAddr)
		{
			g_getNameByReplicationHelper = reinterpret_cast<GetNameByReplicationHelperFn>(nameAddr);
			LOG_DEBUG("ProductionBaseCore: GetBuildingCustomNameByReplicationHelper pattern matched at 0x%llX",
				static_cast<unsigned long long>(nameAddr));
		}
		else
			LOG_WARN("ProductionBaseCore: GetBuildingCustomNameByReplicationHelper pattern not found - "
				"base cores will show as '%s'", kUnnamedBase);

		LOG_INFO("ProductionBaseCore: base core resolution ready (names %s)",
			(g_replicationHelperCtor && g_getNameByReplicationHelper) ? "enabled" : "disabled");
		return true;
	}

	void Shutdown()
	{
		g_getBaseCoresRangedBuilding = nullptr;
		g_replicationHelperCtor      = nullptr;
		g_getNameByReplicationHelper = nullptr;

		std::lock_guard<std::mutex> lock(g_mutex);
		g_buildingBases.clear();
		g_baseLocations.clear();
		g_playerLocationValid = false;
	}

	void OnSessionLoaded()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_buildingBases.clear();
		g_baseLocations.clear();
		g_playerLocationValid = false;
	}

	std::vector<BaseCoreInfo> GetBaseCoresForBuilding(SDK::UWorld* world, ProductionMass::FMassEntityHandle building)
	{
		if (!world || !g_getBaseCoresRangedBuilding)
			return {};

		const uint64_t key = PackHandle(building);
		const auto now = std::chrono::steady_clock::now();

		{
			std::lock_guard<std::mutex> lock(g_mutex);
			auto it = g_buildingBases.find(key);
			if (it != g_buildingBases.end() && now - it->second.resolvedAt < kBaseCacheTtl)
			{
				LOG_TRACE("ProductionBaseCore: cache hit for building Index=%u Serial=%u (%zu base(s))",
					building.Index, building.SerialNumber, it->second.bases.size());
				return it->second.bases;
			}
		}

		LOG_TRACE("ProductionBaseCore: cache miss for building Index=%u Serial=%u - querying",
			building.Index, building.SerialNumber);
		std::vector<BaseCoreInfo> bases = QueryBaseCores(world, building);

		std::lock_guard<std::mutex> lock(g_mutex);
		g_buildingBases[key] = CachedBases{ bases, now };
		return bases;
	}

	void Tick(float deltaSeconds)
	{
		g_playerRefreshTimer += deltaSeconds;
		if (g_playerRefreshTimer < kPlayerRefreshIntervalSeconds)
			return;
		g_playerRefreshTimer = 0.0f;

		SDK::UWorld* world = SDK::UWorld::GetWorld();
		SDK::APawn* pawn = world ? SDK::UGameplayStatics::GetPlayerPawn(world, 0) : nullptr;

		SDK::FVector location{};
		bool valid = false;
		if (pawn)
		{
			location = pawn->K2_GetActorLocation();
			valid = true;
			LOG_TRACE("ProductionBaseCore: player location=(%.0f, %.0f, %.0f)", location.X, location.Y, location.Z);
		}
		else
		{
			LOG_TRACE("ProductionBaseCore: player pawn unavailable (world=%p) - distances will show as unknown",
				static_cast<void*>(world));
		}

		std::lock_guard<std::mutex> lock(g_mutex);
		g_playerLocation = location;
		g_playerLocationValid = valid;
	}

	float GetDistanceMeters(uint64_t baseKey)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_playerLocationValid)
			return -1.0f;

		auto it = g_baseLocations.find(baseKey);
		if (it == g_baseLocations.end())
			return -1.0f;

		const double dx = it->second.X - g_playerLocation.X;
		const double dy = it->second.Y - g_playerLocation.Y;
		const double dz = it->second.Z - g_playerLocation.Z;

		// World units are centimeters.
		return static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0);
	}

	bool GetBaseCoreLocation(uint64_t baseKey, SDK::FVector& outLocation)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_baseLocations.find(baseKey);
		if (it == g_baseLocations.end())
			return false;

		outLocation = it->second;
		return true;
	}
}

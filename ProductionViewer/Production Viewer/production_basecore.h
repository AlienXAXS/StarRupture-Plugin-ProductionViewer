#pragma once

#include "plugin_interface.h"
#include "production_mass.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SDK
{
	class UWorld;
}

// Resolves which base core(s) a Mass building entity resides in, plus each
// base core's player-assigned display name and current distance from the
// player - all without touching any building/base AActor (which is rarely
// spawned for anything the player isn't standing next to).
//
// "Resides in" uses the game's own containment query,
// UCrBaseCoreSubsystem::GetBaseCoresRangedBuilding, which reads the building
// entity's FTransformFragment and asks the base-core spatial grid which core
// areas contain it. Names come from UCrBuildingCustomNameSubsystem (the same
// replicated custom-name store the game's map markers use), mirroring
// BetterMap's basecore_rename.h.
namespace ProductionBaseCore
{
	// One base core a building resides in.
	struct BaseCoreInfo
	{
		ProductionMass::FMassEntityHandle handle{};
		std::string name; // player-assigned custom name, or "Unnamed Base"
	};

	// Packs an entity handle into a single map key. 0 is never produced for a
	// real entity (index 0 is Mass's reserved invalid slot), so callers can use
	// key 0 for their own "unknown" bucket.
	inline uint64_t PackHandle(ProductionMass::FMassEntityHandle handle)
	{
		return (static_cast<uint64_t>(handle.SerialNumber) << 32) | handle.Index;
	}

	// AOB-resolves the base core query + name resolution functions. Returns
	// false (and logs a warning) if the containment query pattern could not be
	// resolved; in that case every lookup returns empty and items report
	// "Unknown Location", but nothing else breaks.
	bool Init(IPluginSelf* self);

	// Clears resolved function pointers and cached data.
	void Shutdown();

	// Drops all cached base core data - call when a save (re)loads, since
	// entity handles and locations don't survive across sessions.
	void OnSessionLoaded();

	// Returns the base core(s) whose area contains `building`, with display
	// names resolved. Results are cached per building for a while (crafters
	// are stationary), so calling this on every craft completion is cheap.
	// Must be called on the game thread. Empty result = "Unknown Location".
	std::vector<BaseCoreInfo> GetBaseCoresForBuilding(SDK::UWorld* world,
		ProductionMass::FMassEntityHandle building);

	// Periodically refreshes the cached player location used by
	// GetDistanceMeters. Call from the engine tick (game thread).
	void Tick(float deltaSeconds);

	// Distance in meters from the player to the given base core (key from
	// PackHandle), or -1.0f if either location is unknown. Safe to call from
	// the UI/render path - only reads cached data.
	float GetDistanceMeters(uint64_t baseKey);

	// Last-known world location of the given base core (key from PackHandle),
	// as of its most recent QueryBaseCores resolution. Returns false if never
	// resolved. Safe to call from the UI/render path - only reads cached data.
	bool GetBaseCoreLocation(uint64_t baseKey, SDK::FVector& outLocation);
}

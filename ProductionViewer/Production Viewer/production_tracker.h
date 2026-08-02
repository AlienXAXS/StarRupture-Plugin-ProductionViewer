#pragma once

#include "plugin_interface.h"
#include "production_data.h"

#include <cstdint>
#include <string>

// Live production/consumption tracking.
//
// Two complementary data sources feed the same item totals:
//  - ACrCrafter::NativeOnItemCraftingComplete (via hooks->Crafting), which
//    only fires for crafters with a loaded actor (i.e. near the player).
//  - ProductionMass's UMassSignalSubsystem::SignalEntity hook, which catches
//    crafting completions for Mass-simulated factories regardless of whether
//    their actor is spawned (see production_mass.h).
//
// Both paths record each finished craft's output item and consumed resources,
// then aggregate them per item name across the 5s / 1m / 10m / 1h / All Time
// windows via TimeSeriesAggregator.
//
// Data is persisted to the active save's session file (see
// ProductionViewer::Storage) so the "All Time" totals survive restarts.
//
// On a multiplayer client neither local source is trusted - the engine only
// simulates what it has streamed in around that player, so the totals would
// silently undercount the rest of the base. Clients instead replay the
// server's feed through the remote-feed entry points at the bottom of this
// header, and everything downstream (sorting, history, the UI) stays the same
// whether the numbers came from this machine or off the wire.
namespace ProductionTracker
{
	// Registers the crafting-finished hook and engine tick. Call once during
	// PluginInit.
	void Init(IPluginSelf* self);

	// Unregisters hooks and flushes any pending data to disk.
	void Shutdown(IPluginSelf* self);

	// Called when a save finishes loading — (re)loads this session's stored
	// production data so "All Time" totals continue from where they left off.
	void OnSessionLoaded();

	// Snapshot of tracked items for the given time range.
	const ProductionData::Category& GetItemsCategory(ProductionData::TimeRange range);

	// ---- Remote feed (multiplayer client) --------------------------------
	// Written only by ProductionNet as it decodes the server's packets.

	// Drops every tracked item. Used when the server changes save.
	void Clear();

	// Reads an item's All Time figures for replication to clients. All three
	// outputs are zeroed if the item isn't tracked.
	void GetAllTimeTotals(const std::string& itemKey, float& outProduced,
		float& outConsumed, float& outElapsedSeconds);

	// Forwards a base core location the server reported to ProductionBaseCore.
	// Routed through here so the network layer stays free of SDK types.
	void SetRemoteBaseLocation(uint64_t baseKey, float x, float y, float z);

	// Replaces an item's All Time totals with the server's, discarding whatever
	// this client had accumulated. Sent on join and on every resync, which is
	// what makes a dropped packet self-correcting rather than permanent drift.
	// The rolling 1m/10m/1h windows are left alone: they are rebuilt from the
	// delta stream and so cover only the time since this client synced.
	void SeedRemoteItem(const std::string& itemKey, const std::string& displayName,
		float allTimeProduced, float allTimeConsumed, float allTimeElapsedSeconds);

	// Adds one broadcast interval's activity to an item's own totals.
	void ApplyRemoteItemDelta(const std::string& itemKey, float produced, float consumed);

	// Adds one broadcast interval's activity to an item's per-base-core
	// breakdown. baseKey 0 is the "Unknown Location" bucket, exactly as in the
	// local path.
	void ApplyRemoteBaseDelta(const std::string& itemKey, uint64_t baseKey,
		const std::string& baseName, float produced, float consumed);
}

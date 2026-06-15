#pragma once

#include "plugin_interface.h"
#include "production_data.h"

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
}

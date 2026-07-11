#pragma once

#include "production_timeseries.h"
#include "plugin_interface.h"

#include <cstdint>
#include <string>
#include <vector>

// Data model for the Production Viewer window.
//
// Categories are populated live by ProductionTracker from the
// ACrCrafter::NativeOnItemCraftingComplete hook.
namespace ProductionData
{
	// This item's activity within one base core's area (or the "Unknown
	// Location" bucket for crafters that resolve to no base core). Shown when
	// the item's row is expanded.
	struct BaseCoreBreakdown
	{
		std::string baseName;    // player-assigned name, "Unnamed Base", or "Unknown Location"
		float total;             // total over the selected time range
		float ratePerMinute;     // current rate
		float distanceMeters;    // player -> base core distance, or -1.0f if unknown
		uint64_t baseKey = 0;    // ProductionBaseCore::PackHandle key, or 0 for "Unknown Location"
	};

	// A single tracked item.
	struct Entry
	{
		std::string name;
		float total;                                // total over the selected time range
		float ratePerMinute;                        // current rate
		std::array<float, kHistorySamples> history;            // sparkline data (raw bucket amounts), oldest -> newest
		std::array<float, kHistorySamples> historyRatePerMinute; // sparkline data as per-minute rates
		PluginTextureHandle icon = nullptr;         // pre-loaded item/recipe icon, or nullptr if unavailable
		std::vector<BaseCoreBreakdown> baseBreakdown; // per-base activity, nearest first (session-scoped)
	};

	// Production/consumption data for the Items view.
	struct Category
	{
		std::string name;
		std::vector<Entry> production;
		std::vector<Entry> consumption;
	};
}

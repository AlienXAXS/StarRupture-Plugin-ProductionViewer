#pragma once

#include "production_timeseries.h"
#include "plugin_interface.h"

#include <string>
#include <vector>

// Data model for the Production Viewer window.
//
// Categories are populated live by ProductionTracker from the
// ACrCrafter::NativeOnItemCraftingComplete hook.
namespace ProductionData
{
	// A single tracked item.
	struct Entry
	{
		std::string name;
		float total;                                // total over the selected time range
		float ratePerMinute;                        // current rate
		std::array<float, kHistorySamples> history;            // sparkline data (raw bucket amounts), oldest -> newest
		std::array<float, kHistorySamples> historyRatePerMinute; // sparkline data as per-minute rates
		PluginTextureHandle icon = nullptr;         // pre-loaded item/recipe icon, or nullptr if unavailable
	};

	// Production/consumption data for the Items view.
	struct Category
	{
		std::string name;
		std::vector<Entry> production;
		std::vector<Entry> consumption;
	};
}

#pragma once

#include <array>
#include <string>
#include <vector>

// Data model for the Production Viewer window.
//
// This is intentionally decoupled from any hook/event source. Once the
// OnCraftingFinished hook lands in the mod loader, something will call into
// ProductionData to record real production/consumption events. Until then,
// GetCategories() returns placeholder sample data so the UI can be built and
// iterated on independently.
namespace ProductionData
{
	// Number of samples kept for the history graph (per item, per time range).
	constexpr int kHistorySamples = 60;

	// A single tracked item/fluid/building/etc.
	struct Entry
	{
		std::string name;
		float total;                                // running total over the selected time range
		float ratePerMinute;                        // current rate
		std::array<float, kHistorySamples> history; // sparkline data, oldest -> newest
	};

	// One tab's worth of data (Items, Fluids, Buildings, Pollution, Kills, ...)
	struct Category
	{
		std::string name;
		std::vector<Entry> production;
		std::vector<Entry> consumption;
	};

	// Time range selector shown along the top of the window.
	enum class TimeRange
	{
		Seconds5,
		Minutes1,
		Minutes10,
		Hours1,
		Hours10,
		Hours50,
		Hours250,
		Hours1000,
		All
	};

	struct TimeRangeOption
	{
		TimeRange value;
		const char* label;
	};

	inline const std::vector<TimeRangeOption>& GetTimeRanges()
	{
		static const std::vector<TimeRangeOption> ranges = {
			{ TimeRange::Seconds5,  "5s" },
			{ TimeRange::Minutes1,  "1m" },
			{ TimeRange::Minutes10, "10m" },
			{ TimeRange::Hours1,    "1h" },
			{ TimeRange::Hours10,   "10h" },
			{ TimeRange::Hours50,   "50h" },
			{ TimeRange::Hours250,  "250h" },
			{ TimeRange::Hours1000, "1000h" },
			{ TimeRange::All,       "All" },
		};
		return ranges;
	}

	// Placeholder/sample data. Replace with live data once production events
	// are wired up via the mod loader's crafting hook.
	const std::vector<Category>& GetCategories();
}

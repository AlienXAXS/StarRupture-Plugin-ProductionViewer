#pragma once

#include "Plugin Core/Vendor/json.hpp"

#include <array>
#include <vector>

namespace ProductionData
{
	// Number of samples kept for the history graph (per item, per time range).
	constexpr int kHistorySamples = 60;

	// Time range selector shown along the top of the window.
	enum class TimeRange
	{
		Minutes1,
		Minutes10,
		Hours1,
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
			{ TimeRange::Minutes1,  "1m" },
			{ TimeRange::Minutes10, "10m" },
			{ TimeRange::Hours1,    "1h" },
			{ TimeRange::All,       "All" },
		};
		return ranges;
	}

	// Tracks production/consumption of a single item across multiple rolling
	// time windows (5s / 1m / 10m / 1h), plus an ever-growing "All Time" window.
	//
	// Implementation: each of the 5s/1m/10m/1h windows is a fixed-size ring
	// buffer of kHistorySamples buckets whose combined width spans exactly the
	// window's duration (e.g. the 1h window has 60 buckets of 1 minute each).
	// The "All Time" window starts with the same bucket width as the 1h window
	// and doubles its bucket width (halving resolution, by merging adjacent
	// buckets pairwise) whenever it fills up — classic round-robin-database
	// downsampling, so it can represent arbitrarily long spans in a fixed-size
	// array.
	class TimeSeriesAggregator
	{
	public:
		// Records an amount that occurred "now" (added to the in-progress bucket
		// of every tier, and to the All-Time running total).
		void AddSample(float amount);

		// Advances all tiers by deltaSeconds, rolling over buckets as needed.
		// Call once per engine tick.
		void Tick(float deltaSeconds);

		// Sum of all samples within the given range's window.
		float GetTotal(TimeRange range) const;

		// GetTotal(range) normalized to a per-minute rate.
		float GetRatePerMinute(TimeRange range) const;

		// Oldest -> newest history samples for the given range's window.
		std::array<float, kHistorySamples> GetHistory(TimeRange range) const;

		// Oldest -> newest history samples converted to per-minute rates.
		std::array<float, kHistorySamples> GetHistoryRatePerMinute(TimeRange range) const;

		// Persists/restores only the All-Time tier and totals — the short-term
		// tiers (5s/1m/10m/1h) intentionally reset each session.
		nlohmann::json ToJson() const;
		void FromJson(const nlohmann::json& json);

	private:
		struct Tier
		{
			float bucketWidth = 1.0f;
			std::array<float, kHistorySamples> buckets{};
			float currentAccum = 0.0f;
			float currentElapsed = 0.0f;
			int writeIndex = 0; // index of the oldest bucket / next to be overwritten
		};

		static void TickTier(Tier& tier, float deltaSeconds, bool allowGrow);
		static float TierTotal(const Tier& tier);
		static std::array<float, kHistorySamples> TierHistory(const Tier& tier);
		static const Tier& FixedTierForRange(const TimeSeriesAggregator& self, TimeRange range);

		// Fixed-width tiers covering 1m, 10m, 1h (index matches TimeRange
		// ordering for the windowed ranges).
		Tier m_fixedTiers[3] = {
			Tier{ 60.0f / kHistorySamples },
			Tier{ 600.0f / kHistorySamples },
			Tier{ 3600.0f / kHistorySamples },
		};

		// Adaptive-resolution tier covering the entire session/save history.
		Tier m_allTimeTier{ 3600.0f / kHistorySamples };

		float m_allTimeTotal = 0.0f;
		float m_allTimeElapsed = 0.0f; // seconds, accumulated across all sessions
	};
}

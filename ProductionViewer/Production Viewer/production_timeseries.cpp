#include "production_timeseries.h"

namespace ProductionData
{
	void TimeSeriesAggregator::AddSample(float amount)
	{
		for (Tier& tier : m_fixedTiers)
			tier.currentAccum += amount;

		m_allTimeTier.currentAccum += amount;
		m_allTimeTotal += amount;
	}

	void TimeSeriesAggregator::Tick(float deltaSeconds)
	{
		for (Tier& tier : m_fixedTiers)
			TickTier(tier, deltaSeconds, false);

		TickTier(m_allTimeTier, deltaSeconds, true);
		m_allTimeElapsed += deltaSeconds;
	}

	void TimeSeriesAggregator::TickTier(Tier& tier, float deltaSeconds, bool allowGrow)
	{
		tier.currentElapsed += deltaSeconds;

		while (tier.currentElapsed >= tier.bucketWidth)
		{
			tier.currentElapsed -= tier.bucketWidth;
			tier.buckets[tier.writeIndex] = tier.currentAccum;
			tier.currentAccum = 0.0f;
			tier.writeIndex = (tier.writeIndex + 1) % kHistorySamples;

			// Ring buffer just filled — halve resolution (merge bucket pairs),
			// double the bucket width, and continue writing from the midpoint.
			if (allowGrow && tier.writeIndex == 0)
			{
				std::array<float, kHistorySamples> merged{};
				for (int i = 0; i < kHistorySamples / 2; ++i)
					merged[i] = tier.buckets[2 * i] + tier.buckets[2 * i + 1];

				tier.buckets = merged;
				tier.bucketWidth *= 2.0f;
				tier.writeIndex = kHistorySamples / 2;
			}
		}
	}

	float TimeSeriesAggregator::TierTotal(const Tier& tier)
	{
		float total = tier.currentAccum;
		for (float sample : tier.buckets)
			total += sample;
		return total;
	}

	std::array<float, kHistorySamples> TimeSeriesAggregator::TierHistory(const Tier& tier)
	{
		std::array<float, kHistorySamples> out{};
		for (int i = 0; i < kHistorySamples; ++i)
			out[i] = tier.buckets[(tier.writeIndex + i) % kHistorySamples];

		// Fold the in-progress bucket into the most recent sample for a "live" feel.
		out[kHistorySamples - 1] += tier.currentAccum;
		return out;
	}

	const TimeSeriesAggregator::Tier& TimeSeriesAggregator::FixedTierForRange(const TimeSeriesAggregator& self, TimeRange range)
	{
		switch (range)
		{
			case TimeRange::Minutes1:  return self.m_fixedTiers[0];
			case TimeRange::Minutes10: return self.m_fixedTiers[1];
			case TimeRange::Hours1:    return self.m_fixedTiers[2];
			default:                   return self.m_allTimeTier;
		}
	}

	float TimeSeriesAggregator::GetTotal(TimeRange range) const
	{
		if (range == TimeRange::All)
			return m_allTimeTotal;

		return TierTotal(FixedTierForRange(*this, range));
	}

	float TimeSeriesAggregator::GetRatePerMinute(TimeRange range) const
	{
		if (range == TimeRange::All)
			return m_allTimeElapsed > 0.0f ? (m_allTimeTotal / m_allTimeElapsed) * 60.0f : 0.0f;

		const Tier& tier = FixedTierForRange(*this, range);
		const float windowSeconds = tier.bucketWidth * kHistorySamples;
		return windowSeconds > 0.0f ? (GetTotal(range) / windowSeconds) * 60.0f : 0.0f;
	}

	std::array<float, kHistorySamples> TimeSeriesAggregator::GetHistory(TimeRange range) const
	{
		return TierHistory(FixedTierForRange(*this, range));
	}

	std::array<float, kHistorySamples> TimeSeriesAggregator::GetHistoryRatePerMinute(TimeRange range) const
	{
		const Tier& tier = FixedTierForRange(*this, range);
		const float scale = tier.bucketWidth > 0.0f ? 60.0f / tier.bucketWidth : 0.0f;
		auto raw = TierHistory(tier);
		for (float& v : raw)
			v *= scale;
		return raw;
	}

	nlohmann::json TimeSeriesAggregator::ToJson() const
	{
		nlohmann::json j;
		j["allTimeTotal"] = m_allTimeTotal;
		j["allTimeElapsed"] = m_allTimeElapsed;
		j["bucketWidth"] = m_allTimeTier.bucketWidth;
		j["writeIndex"] = m_allTimeTier.writeIndex;
		j["currentAccum"] = m_allTimeTier.currentAccum;
		j["currentElapsed"] = m_allTimeTier.currentElapsed;
		j["buckets"] = m_allTimeTier.buckets;
		return j;
	}

	void TimeSeriesAggregator::FromJson(const nlohmann::json& json)
	{
		m_allTimeTotal = json.value("allTimeTotal", 0.0f);
		m_allTimeElapsed = json.value("allTimeElapsed", 0.0f);
		m_allTimeTier.bucketWidth = json.value("bucketWidth", 3600.0f / kHistorySamples);
		m_allTimeTier.writeIndex = json.value("writeIndex", 0);
		m_allTimeTier.currentAccum = json.value("currentAccum", 0.0f);
		m_allTimeTier.currentElapsed = json.value("currentElapsed", 0.0f);

		if (json.contains("buckets") && json["buckets"].is_array() && json["buckets"].size() == kHistorySamples)
		{
			for (int i = 0; i < kHistorySamples; ++i)
				m_allTimeTier.buckets[i] = json["buckets"][i].get<float>();
		}
	}
}

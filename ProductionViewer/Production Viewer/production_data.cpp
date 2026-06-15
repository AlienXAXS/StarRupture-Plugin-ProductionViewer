#include "production_data.h"

#include <cmath>

namespace ProductionData
{
	namespace
	{
		// Builds a gently fluctuating sparkline so the placeholder graphs look
		// "alive" without depending on real production data.
		std::array<float, kHistorySamples> MakeHistory(float base, float amplitude, float phase)
		{
			std::array<float, kHistorySamples> samples{};
			for (int i = 0; i < kHistorySamples; ++i)
			{
				float t = static_cast<float>(i);
				samples[i] = base + amplitude * std::sin((t * 0.25f) + phase);
				if (samples[i] < 0.0f)
					samples[i] = 0.0f;
			}
			return samples;
		}

		Entry MakeEntry(const char* name, float total, float ratePerMinute, float amplitude, float phase)
		{
			Entry entry;
			entry.name = name;
			entry.total = total;
			entry.ratePerMinute = ratePerMinute;
			entry.history = MakeHistory(ratePerMinute, amplitude, phase);
			return entry;
		}
	}

	const std::vector<Category>& GetCategories()
	{
		static const std::vector<Category> categories = {
			{
				"Items",
				// Production
				{
					MakeEntry("Iron Plate",     5800.0f, 5900.0f, 400.0f, 0.0f),
					MakeEntry("Iron Ore",       2400.0f, 2500.0f, 250.0f, 0.6f),
					MakeEntry("Copper Plate",   1300.0f, 1300.0f, 150.0f, 1.2f),
					MakeEntry("Iron Gear Wheel",1000.0f, 1100.0f, 120.0f, 1.8f),
					MakeEntry("Copper Cable",    684.0f,  684.0f,  80.0f, 2.4f),
					MakeEntry("Electronic Circuit", 362.0f, 362.0f, 60.0f, 3.0f),
					MakeEntry("Steel Plate",     256.0f,  256.0f,  40.0f, 3.6f),
					MakeEntry("Concrete",        188.0f,  188.0f,  30.0f, 4.2f),
					MakeEntry("Engine Unit",     128.0f,  128.0f,  20.0f, 4.8f),
				},
				// Consumption
				{
					MakeEntry("Iron Plate",     5800.0f, 5900.0f, 400.0f, 0.1f),
					MakeEntry("Iron Ore",       5200.0f, 5300.0f, 350.0f, 0.7f),
					MakeEntry("Copper Plate",   2400.0f, 2500.0f, 250.0f, 1.3f),
					MakeEntry("Coal",           1300.0f, 1400.0f, 150.0f, 1.9f),
					MakeEntry("Copper Cable",   1300.0f, 1300.0f, 130.0f, 2.5f),
					MakeEntry("Iron Gear Wheel", 1000.0f, 1000.0f, 100.0f, 3.1f),
					MakeEntry("Plastic Bar",      922.0f,  922.0f,  90.0f, 3.7f),
					MakeEntry("Electronic Circuit", 553.0f, 553.0f, 60.0f, 4.3f),
					MakeEntry("Stone",            258.0f,  258.0f, 40.0f, 4.9f),
				},
			},
			{ "Fluids",     {}, {} },
			{ "Buildings",  {}, {} },
			{ "Pollution",  {}, {} },
			{ "Kills",      {}, {} },
		};
		return categories;
	}
}

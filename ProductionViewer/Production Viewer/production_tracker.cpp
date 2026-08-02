#include "production_tracker.h"
#include "production_timeseries.h"
#include "production_icons.h"
#include "production_mass.h"
#include "production_basecore.h"
#include "Plugin Core/Helpers/plugin_helpers.h"
#include "Plugin Core/Net/production_net.h"
#include "Plugin Core/Signatures/plugin_signatures.h"
#include "Plugin Core/Storage/production_storage.h"

#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"
#include "AuCrafting_classes.hpp"
#include "AuCrafting_structs.hpp"
#include "AuItems_classes.hpp"
#include "AuItems_structs.hpp"

#include <algorithm>
#include <map>
#include <mutex>

using namespace ProductionData;

namespace ProductionTracker
{
	namespace
	{
		// How often the running totals are flushed to the session file.
		constexpr float kSaveIntervalSeconds = 30.0f;

		// This item's activity attributed to one base core (or the "Unknown
		// Location" bucket for crafters outside any base core's area).
		struct PerBaseRecord
		{
			std::string baseName;
			TimeSeriesAggregator production;
			TimeSeriesAggregator consumption;
		};

		struct ItemRecord
		{
			std::string displayName;
			TimeSeriesAggregator production;
			TimeSeriesAggregator consumption;
			// Keyed by packed base core handle (ProductionBaseCore::PackHandle);
			// key 0 = "Unknown Location". Session-scoped - unlike the global
			// aggregators above, this is never persisted, so the per-base view
			// resets each session (entity handles don't survive reloads anyway).
			std::map<uint64_t, PerBaseRecord> perBase;
		};

		std::mutex g_mutex;
		std::map<std::string, ItemRecord> g_items; // key: UniqueItemName
		float g_saveTimer = 0.0f;

		// The session file is only loaded once the session role is known: a
		// multiplayer client must not restore (or later overwrite) a local
		// save's totals with the server's numbers, and the role isn't resolved
		// yet when the experience finishes loading.
		bool g_sessionLoadPending = false;

		// Resolves a stable key + display name for an item, falling back to the
		// key itself if no localized display name is available.
		void ResolveItemNames(SDK::UAuItemDataBase* itemData, std::string& outKey, std::string& outDisplayName)
		{
			if (!itemData)
			{
				outKey = "Unknown";
				outDisplayName = "Unknown";
				return;
			}

			outKey = itemData->UniqueItemName.ToString();
			outDisplayName = SDK::UKismetTextLibrary::Conv_TextToString(itemData->ItemName).ToString();

			if (outKey.empty())
				outKey = "Unknown";
			if (outDisplayName.empty())
				outDisplayName = outKey;
		}

		void RecordSample(SDK::UAuItemDataBase* itemData, float amount, bool isProduction,
			const std::vector<ProductionBaseCore::BaseCoreInfo>& bases)
		{
			if (amount <= 0.0f)
				return;

			std::string key, displayName;
			ResolveItemNames(itemData, key, displayName);

			// One flattened description of where this craft happened, used for
			// both the local aggregation and the wire. An empty `bases` means
			// the crafter resolved to no base core area at all.
			std::vector<ProductionNet::BaseSample> samples;
			samples.reserve((std::max)(bases.size(), static_cast<size_t>(1)));

			auto addSample = [&samples](uint64_t baseKey, const std::string& baseName)
			{
				ProductionNet::BaseSample sample;
				sample.key = baseKey;
				sample.name = baseName;

				SDK::FVector location{};
				if (baseKey != 0 && ProductionBaseCore::GetBaseCoreLocation(baseKey, location))
				{
					sample.hasLocation = true;
					sample.locX = static_cast<float>(location.X);
					sample.locY = static_cast<float>(location.Y);
					sample.locZ = static_cast<float>(location.Z);
				}

				samples.push_back(std::move(sample));
			};

			if (bases.empty())
				addSample(0, "Unknown Location");
			else
				for (const ProductionBaseCore::BaseCoreInfo& base : bases)
					addSample(ProductionBaseCore::PackHandle(base.handle), base.name);

			{
				std::lock_guard<std::mutex> lock(g_mutex);
				ItemRecord& record = g_items[key];
				if (record.displayName.empty())
					record.displayName = displayName;

				if (isProduction)
					record.production.AddSample(amount);
				else
					record.consumption.AddSample(amount);

				for (const ProductionNet::BaseSample& sample : samples)
				{
					PerBaseRecord& perBase = record.perBase[sample.key];
					perBase.baseName = sample.name; // refreshed every sample so renames propagate
					if (isProduction)
						perBase.production.AddSample(amount);
					else
						perBase.consumption.AddSample(amount);
				}
			}

			// Deliberately outside the lock above: the network layer takes its
			// own, and the client-side apply path runs in the opposite order.
			ProductionNet::OnLocalSample(key, displayName, amount, isProduction, samples);
		}

		// Builds the persisted JSON blob for all tracked items.
		nlohmann::json BuildSaveData()
		{
			nlohmann::json items = nlohmann::json::object();

			std::lock_guard<std::mutex> lock(g_mutex);
			for (const auto& [key, record] : g_items)
			{
				nlohmann::json entry;
				entry["displayName"] = record.displayName;
				entry["production"] = record.production.ToJson();
				entry["consumption"] = record.consumption.ToJson();
				items[key] = entry;
			}

			return items;
		}

		void SaveToSession()
		{
			if (!ProductionViewer::Storage::IsLoaded())
				return;

			// A client's totals belong to the server's world, not to whatever
			// local save the session name happens to resolve to - writing them
			// out would quietly overwrite the player's own history.
			if (ProductionNet::IsRemoteClient())
				return;

			ProductionViewer::Storage::Set("items", BuildSaveData());
			ProductionViewer::Storage::Save();
		}

		void LoadFromSession()
		{
			nlohmann::json items = ProductionViewer::Storage::Get("items", nlohmann::json::object());
			if (!items.is_object())
				return;

			std::lock_guard<std::mutex> lock(g_mutex);
			for (auto it = items.begin(); it != items.end(); ++it)
			{
				ItemRecord& record = g_items[it.key()];
				const nlohmann::json& entry = it.value();

				record.displayName = entry.value("displayName", it.key());
				if (entry.contains("production"))
					record.production.FromJson(entry["production"]);
				if (entry.contains("consumption"))
					record.consumption.FromJson(entry["consumption"]);
			}

			LOG_INFO("ProductionTracker: restored %zu item(s) from session '%s'.",
				g_items.size(), ProductionViewer::Storage::GetSessionName().c_str());
		}

		// Fired for every Mass crafting entity (loaded or simulated/de-spawned) whose
		// crafting just completed, via ProductionMass's SignalEntity hook. Mirrors
		// OnCraftingFinished but reads the recipe straight from the Mass fragment
		// instead of the (actor-only) crafting component.
		// Rejects pointers that are obviously not real heap addresses (null,
		// below the first page, or above the x64 user-mode VA ceiling) before
		// they're dereferenced/passed to ProcessEvent - guards against
		// misread Mass fragment offsets.
		bool LooksLikeValidPointer(const void* ptr)
		{
			auto addr = reinterpret_cast<uintptr_t>(ptr);
			return addr > 0x10000 && addr < 0x0000800000000000ULL;
		}

		void OnMassCraftingComplete(const SDK::FCrCraftingFragment* fragment, SDK::UWorld* world,
			ProductionMass::FMassEntityHandle entity)
		{
			// A client only simulates the crafters the engine has streamed in
			// around it, so anything counted here would be a fraction of the
			// base presented as the whole of it. The server's feed is the only
			// source on a client - bail before the base core query, which is
			// the expensive part of this path.
			if (ProductionNet::IsRemoteClient())
				return;

			LOG_DEBUG("ProductionTracker: OnMassCraftingComplete - fragment=%p CurrentRecipe=%p SelectedRecipe=%p CraftingMultiplier=%d",
				fragment, fragment->CurrentRecipe, fragment->SelectedRecipe, fragment->CraftingMultiplier);

			SDK::UCrItemRecipeData* recipe = fragment->CurrentRecipe ? fragment->CurrentRecipe : fragment->SelectedRecipe;
			if (!recipe)
			{
				LOG_DEBUG("ProductionTracker: OnMassCraftingComplete - no recipe (CurrentRecipe/SelectedRecipe both null), ignoring");
				return;
			}

			if (!LooksLikeValidPointer(recipe))
			{
				LOG_WARN("ProductionTracker: OnMassCraftingComplete - recipe pointer %p looks invalid, ignoring "
					"(fragment offsets may be wrong)", recipe);
				return;
			}

			const int32_t multiplier = fragment->CraftingMultiplier > 0 ? fragment->CraftingMultiplier : 1;

			// Which base core(s) this crafter resides in - cached per crafter,
			// so this is a map lookup on all but the first (and TTL-refresh)
			// completions. Empty = "Unknown Location".
			const std::vector<ProductionBaseCore::BaseCoreInfo> bases =
				ProductionBaseCore::GetBaseCoresForBuilding(world, entity);

			LOG_DEBUG("ProductionTracker: OnMassCraftingComplete - crafter Index=%u Serial=%u resides in %zu base(s)%s%s",
				entity.Index, entity.SerialNumber, bases.size(),
				bases.empty() ? "" : ", first: ", bases.empty() ? "" : bases[0].name.c_str());

			LOG_DEBUG("ProductionTracker: OnMassCraftingComplete - calling GetOutputItem on recipe=%p", recipe);
			SDK::FAuSimpleItem outputItem = recipe->GetOutputItem();
			std::string outputKey, outputDisplayName;
			ResolveItemNames(outputItem.ItemDataBase, outputKey, outputDisplayName);
			LOG_DEBUG("ProductionTracker: OnMassCraftingComplete - output '%s' (%s) x%d (multiplier %d)",
				outputDisplayName.c_str(), outputKey.c_str(), outputItem.Count, multiplier);

			RecordSample(outputItem.ItemDataBase, static_cast<float>(outputItem.Count * multiplier), true, bases);

			for (const SDK::FAuSimpleItem& resource : recipe->GetNeededResources())
				RecordSample(resource.ItemDataBase, static_cast<float>(resource.Count * multiplier), false, bases);
		}

		// Restores the session file once the role is known. Deferred because
		// OnExperienceLoadComplete fires before an actor exists to ask for the
		// net mode, and a client must restore nothing.
		void ServiceDeferredSessionLoad()
		{
			if (!g_sessionLoadPending)
				return;
			if (ProductionNet::GetRole() == ProductionNet::SessionRole::Unresolved)
				return;

			g_sessionLoadPending = false;

			if (ProductionNet::IsRemoteClient())
			{
				LOG_INFO("ProductionTracker: multiplayer client - production data will come from the server");
				return;
			}

			LoadFromSession();
		}

		void OnEngineTick(float deltaSeconds)
		{
			ProductionIcons::Tick();
			ProductionBaseCore::Tick(deltaSeconds);
			ProductionNet::Tick(deltaSeconds);
			ServiceDeferredSessionLoad();

			{
				std::lock_guard<std::mutex> lock(g_mutex);
				for (auto& [key, record] : g_items)
				{
					record.production.Tick(deltaSeconds);
					record.consumption.Tick(deltaSeconds);

					for (auto& [baseKey, perBase] : record.perBase)
					{
						perBase.production.Tick(deltaSeconds);
						perBase.consumption.Tick(deltaSeconds);
					}
				}
			}

			g_saveTimer += deltaSeconds;
			if (g_saveTimer >= kSaveIntervalSeconds)
			{
				g_saveTimer = 0.0f;
				SaveToSession();
			}
		}
	}

	void Init(IPluginSelf* self)
	{
		if (self->hooks->Engine)
			self->hooks->Engine->RegisterOnTick(&OnEngineTick);

		// Resolves the base core containment query + custom-name functions used
		// to attribute craft completions to a base. Degrades to "Unknown
		// Location" if its patterns don't match.
		ProductionBaseCore::Init(self);

		// Catches crafting completions for Mass-simulated (de-spawned) factories,
		// which the actor-only hook above can't see.
		ProductionMass::Init(self, &OnMassCraftingComplete);

		LOG_INFO("ProductionTracker: initialized");
	}

	void Shutdown(IPluginSelf* self)
	{
		if (self->hooks->Engine)
			self->hooks->Engine->UnregisterOnTick(&OnEngineTick);

		ProductionMass::Shutdown(self);
		ProductionBaseCore::Shutdown();

		SaveToSession();

		std::lock_guard<std::mutex> lock(g_mutex);
		g_items.clear();
	}

	void OnSessionLoaded()
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_items.clear();
			g_saveTimer = 0.0f;
			g_sessionLoadPending = true;
		}

		// Entity handles and cached locations don't survive a session change.
		ProductionBaseCore::OnSessionLoaded();

		// Clients are told to drop what they hold; the restore itself waits for
		// the session role (see ServiceDeferredSessionLoad).
		ProductionNet::OnSessionLoaded();
	}

	const Category& GetItemsCategory(TimeRange range)
	{
		static Category items;

		items = Category{};
		items.name = "Items";

		// Per-base breakdown for one direction (production or consumption) of
		// an item, nearest base first, unknown distances last.
		auto buildBreakdown = [range](const std::map<uint64_t, PerBaseRecord>& perBase, bool isProduction)
		{
			std::vector<BaseCoreBreakdown> breakdown;
			for (const auto& [baseKey, record] : perBase)
			{
				const TimeSeriesAggregator& agg = isProduction ? record.production : record.consumption;
				const float total = agg.GetTotal(range);
				if (total <= 0.0f)
					continue;

				BaseCoreBreakdown base;
				base.baseName = record.baseName;
				base.total = total;
				base.ratePerMinute = agg.GetRatePerMinute(TimeRange::Minutes1);
				base.distanceMeters = baseKey != 0 ? ProductionBaseCore::GetDistanceMeters(baseKey) : -1.0f;
				base.baseKey = baseKey;
				breakdown.push_back(std::move(base));
			}

			std::sort(breakdown.begin(), breakdown.end(),
				[](const BaseCoreBreakdown& a, const BaseCoreBreakdown& b)
				{
					const bool aKnown = a.distanceMeters >= 0.0f;
					const bool bKnown = b.distanceMeters >= 0.0f;
					if (aKnown != bKnown)
						return aKnown;
					if (aKnown && a.distanceMeters != b.distanceMeters)
						return a.distanceMeters < b.distanceMeters;
					return a.baseName < b.baseName;
				});

			return breakdown;
		};

		{
			std::lock_guard<std::mutex> lock(g_mutex);
			items.production.reserve(g_items.size());
			items.consumption.reserve(g_items.size());

			for (const auto& [key, record] : g_items)
			{
				PluginTextureHandle icon = ProductionIcons::GetIcon(key);

				if (record.production.GetTotal(TimeRange::All) > 0.0f)
				{
					Entry entry;
					entry.name = record.displayName;
					entry.total = record.production.GetTotal(range);
					entry.ratePerMinute = record.production.GetRatePerMinute(TimeRange::Minutes1);
					entry.history = record.production.GetHistory(range);
					entry.historyRatePerMinute = record.production.GetHistoryRatePerMinute(range);
					entry.icon = icon;
					entry.baseBreakdown = buildBreakdown(record.perBase, true);
					items.production.push_back(std::move(entry));
				}

				if (record.consumption.GetTotal(TimeRange::All) > 0.0f)
				{
					Entry entry;
					entry.name = record.displayName;
					entry.total = record.consumption.GetTotal(range);
					entry.ratePerMinute = record.consumption.GetRatePerMinute(TimeRange::Minutes1);
					entry.history = record.consumption.GetHistory(range);
					entry.historyRatePerMinute = record.consumption.GetHistoryRatePerMinute(range);
					entry.icon = icon;
					entry.baseBreakdown = buildBreakdown(record.perBase, false);
					items.consumption.push_back(std::move(entry));
				}
			}
		}

		auto byTotalDesc = [](const Entry& a, const Entry& b) { return a.total > b.total; };
		std::sort(items.production.begin(), items.production.end(), byTotalDesc);
		std::sort(items.consumption.begin(), items.consumption.end(), byTotalDesc);

		return items;
	}

	void Clear()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_items.clear();
	}

	void GetAllTimeTotals(const std::string& itemKey, float& outProduced,
		float& outConsumed, float& outElapsedSeconds)
	{
		outProduced = 0.0f;
		outConsumed = 0.0f;
		outElapsedSeconds = 0.0f;

		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_items.find(itemKey);
		if (it == g_items.end())
			return;

		outProduced = it->second.production.GetTotal(TimeRange::All);
		outConsumed = it->second.consumption.GetTotal(TimeRange::All);

		// Both aggregators are ticked together, so either one describes how
		// long the item has been tracked.
		outElapsedSeconds = it->second.production.GetAllTimeElapsed();
	}

	void SetRemoteBaseLocation(uint64_t baseKey, float x, float y, float z)
	{
		ProductionBaseCore::SetRemoteBaseLocation(baseKey, x, y, z);
	}

	void SeedRemoteItem(const std::string& itemKey, const std::string& displayName,
		float allTimeProduced, float allTimeConsumed, float allTimeElapsedSeconds)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		ItemRecord& record = g_items[itemKey];
		record.displayName = displayName;
		record.production.SeedAllTime(allTimeProduced, allTimeElapsedSeconds);
		record.consumption.SeedAllTime(allTimeConsumed, allTimeElapsedSeconds);
	}

	void ApplyRemoteItemDelta(const std::string& itemKey, float produced, float consumed)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_items.find(itemKey);
		if (it == g_items.end())
			return;   // never defined by the server; nothing to attribute this to

		if (produced > 0.0f)
			it->second.production.AddSample(produced);
		if (consumed > 0.0f)
			it->second.consumption.AddSample(consumed);
	}

	void ApplyRemoteBaseDelta(const std::string& itemKey, uint64_t baseKey,
		const std::string& baseName, float produced, float consumed)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_items.find(itemKey);
		if (it == g_items.end())
			return;

		PerBaseRecord& perBase = it->second.perBase[baseKey];
		perBase.baseName = baseName;
		if (produced > 0.0f)
			perBase.production.AddSample(produced);
		if (consumed > 0.0f)
			perBase.consumption.AddSample(consumed);
	}
}

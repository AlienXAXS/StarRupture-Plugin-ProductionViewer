#include "production_tracker.h"
#include "production_timeseries.h"
#include "production_icons.h"
#include "production_mass.h"
#include "Plugin Core/Helpers/plugin_helpers.h"
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

		struct ItemRecord
		{
			std::string displayName;
			TimeSeriesAggregator production;
			TimeSeriesAggregator consumption;
		};

		std::mutex g_mutex;
		std::map<std::string, ItemRecord> g_items; // key: UniqueItemName
		float g_saveTimer = 0.0f;

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

		void RecordSample(SDK::UAuItemDataBase* itemData, float amount, bool isProduction)
		{
			if (amount <= 0.0f)
				return;

			std::string key, displayName;
			ResolveItemNames(itemData, key, displayName);

			std::lock_guard<std::mutex> lock(g_mutex);
			ItemRecord& record = g_items[key];
			if (record.displayName.empty())
				record.displayName = displayName;

			if (isProduction)
				record.production.AddSample(amount);
			else
				record.consumption.AddSample(amount);
		}

		// UCrCraftingComponent::GetCraftingFragment(UCrCraftingComponent*)
		// Returns the live FCrCraftingFragment* holding CurrentRecipe/SelectedRecipe.
		// Used as a fallback when ItemsToCraft has already been drained by the
		// time OnCraftingFinished fires (mirrors WAILA's crafter_detector).
		using GetCraftingFragmentFn = const SDK::FCrCraftingFragment* (__fastcall*)(SDK::UCrCraftingComponent*);

		GetCraftingFragmentFn g_getCraftingFragment = nullptr;

		// AOB-resolves UCrCraftingComponent::GetCraftingFragment. Called once from
		// Init so the scan happens up front rather than on the first crafting
		// completion.
		void ResolveGetCraftingFragment()
		{
			IPluginScanner* scanner = GetScanner();
			if (!scanner)
				return;

			uintptr_t addr = scanner->FindPatternInMainModule(Signatures::GetCraftingFragment);
			if (!addr)
			{
				LOG_WARN("ProductionTracker: GetCraftingFragment pattern not found - "
					"recipe lookup will be unavailable when ItemsToCraft is empty");
				return;
			}

			g_getCraftingFragment = reinterpret_cast<GetCraftingFragmentFn>(addr);
		}

		// Falls back to the crafting component's live fragment (CurrentRecipe,
		// then SelectedRecipe) when ItemsToCraft has already been drained.
		bool RecordFromCraftingFragment(SDK::UCrCraftingComponent* comp)
		{
			if (!g_getCraftingFragment)
				return false;

			const SDK::FCrCraftingFragment* fragment = g_getCraftingFragment(comp);
			if (!fragment)
				return false;

			SDK::UCrItemRecipeData* recipe = fragment->CurrentRecipe ? fragment->CurrentRecipe : fragment->SelectedRecipe;
			if (!recipe)
				return false;

			SDK::FAuSimpleItem outputItem = recipe->GetOutputItem();
			std::string outputKey, outputDisplayName;
			ResolveItemNames(outputItem.ItemDataBase, outputKey, outputDisplayName);
			LOG_DEBUG("ProductionTracker: OnCraftingFinished - output '%s' (%s) x%d via crafting fragment fallback",
				outputDisplayName.c_str(), outputKey.c_str(), outputItem.Count);

			RecordSample(outputItem.ItemDataBase, static_cast<float>(outputItem.Count), true);

			for (const SDK::FAuSimpleItem& resource : recipe->GetNeededResources())
				RecordSample(resource.ItemDataBase, static_cast<float>(resource.Count), false);

			return true;
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

		// Fired whenever any crafting building finishes crafting an item.
		// Records the recipe's output as production and its consumed
		// resources as consumption for the current craft.
		void OnCraftingFinished(void* /*crafter*/, void* craftingComponent, int32_t /*entityIndex*/, int32_t /*entitySerial*/)
		{
			LOG_DEBUG("ProductionTracker: OnCraftingFinished fired (craftingComponent=%p)", craftingComponent);

			auto* comp = static_cast<SDK::UCrCraftingComponent*>(craftingComponent);
			if (!comp)
			{
				LOG_DEBUG("ProductionTracker: OnCraftingFinished - craftingComponent is null, ignoring");
				return;
			}

			const SDK::TArray<SDK::FAuCraftItem>& items = comp->ItemsToCraft;
			if (items.Num() == 0)
			{
				// ItemsToCraft is only populated while a craft is in flight; by the
				// time this event fires it may already have been drained. Fall back
				// to the crafting component's live fragment to find the recipe.
				if (!RecordFromCraftingFragment(comp))
					LOG_DEBUG("ProductionTracker: OnCraftingFinished - ItemsToCraft is empty and no crafting fragment recipe found, ignoring");

				return;
			}

			const SDK::FAuCraftItem& craftItem = items[0];

			// OutputItem.ItemDataBase isn't always populated on the instance struct;
			// fall back to the recipe's item data (mirrors WAILA's crafter_detector).
			SDK::UAuItemDataBase* outputItemData = craftItem.OutputItem.ItemDataBase;
			if (!outputItemData && craftItem.RecipeData)
				outputItemData = craftItem.RecipeData->GetItemDataBase();

			std::string outputKey, outputDisplayName;
			ResolveItemNames(outputItemData, outputKey, outputDisplayName);
			LOG_DEBUG("ProductionTracker: OnCraftingFinished - output '%s' (%s) x%d, %d resource(s) consumed",
				outputDisplayName.c_str(), outputKey.c_str(), craftItem.OutputItem.Count, craftItem.NeededResources.Num());

			RecordSample(outputItemData, static_cast<float>(craftItem.OutputItem.Count), true);

			for (const SDK::FAuSimpleItem& resource : craftItem.NeededResources)
				RecordSample(resource.ItemDataBase, static_cast<float>(resource.Count), false);
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

		void OnMassCraftingComplete(const SDK::FCrCraftingFragment* fragment)
		{
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

			LOG_DEBUG("ProductionTracker: OnMassCraftingComplete - calling GetOutputItem on recipe=%p", recipe);
			SDK::FAuSimpleItem outputItem = recipe->GetOutputItem();
			std::string outputKey, outputDisplayName;
			ResolveItemNames(outputItem.ItemDataBase, outputKey, outputDisplayName);
			LOG_DEBUG("ProductionTracker: OnMassCraftingComplete - output '%s' (%s) x%d (multiplier %d)",
				outputDisplayName.c_str(), outputKey.c_str(), outputItem.Count, multiplier);

			RecordSample(outputItem.ItemDataBase, static_cast<float>(outputItem.Count * multiplier), true);

			for (const SDK::FAuSimpleItem& resource : recipe->GetNeededResources())
				RecordSample(resource.ItemDataBase, static_cast<float>(resource.Count * multiplier), false);
		}

		void OnEngineTick(float deltaSeconds)
		{
			ProductionIcons::Tick();

			{
				std::lock_guard<std::mutex> lock(g_mutex);
				for (auto& [key, record] : g_items)
				{
					record.production.Tick(deltaSeconds);
					record.consumption.Tick(deltaSeconds);
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
		ResolveGetCraftingFragment();

		if (self->hooks->Crafting)
		{
			self->hooks->Crafting->RegisterOnCraftingFinished(&OnCraftingFinished);
		}
		else
		{
			LOG_WARN("ProductionTracker: crafting hooks unavailable - production will not be tracked");
		}

		if (self->hooks->Engine)
			self->hooks->Engine->RegisterOnTick(&OnEngineTick);

		// Catches crafting completions for Mass-simulated (de-spawned) factories,
		// which the actor-only hook above can't see.
		ProductionMass::Init(self, &OnMassCraftingComplete);

		LOG_INFO("ProductionTracker: initialized");
	}

	void Shutdown(IPluginSelf* self)
	{
		if (self->hooks->Crafting)
			self->hooks->Crafting->UnregisterOnCraftingFinished(&OnCraftingFinished);

		if (self->hooks->Engine)
			self->hooks->Engine->UnregisterOnTick(&OnEngineTick);

		ProductionMass::Shutdown(self);

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
		}

		LoadFromSession();
	}

	const Category& GetItemsCategory(TimeRange range)
	{
		static Category items;

		items = Category{};
		items.name = "Items";

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
					entry.ratePerMinute = record.production.GetRatePerMinute(range);
					entry.history = record.production.GetHistory(range);
					entry.icon = icon;
					items.production.push_back(std::move(entry));
				}

				if (record.consumption.GetTotal(TimeRange::All) > 0.0f)
				{
					Entry entry;
					entry.name = record.displayName;
					entry.total = record.consumption.GetTotal(range);
					entry.ratePerMinute = record.consumption.GetRatePerMinute(range);
					entry.history = record.consumption.GetHistory(range);
					entry.icon = icon;
					items.consumption.push_back(std::move(entry));
				}
			}
		}

		auto byTotalDesc = [](const Entry& a, const Entry& b) { return a.total > b.total; };
		std::sort(items.production.begin(), items.production.end(), byTotalDesc);
		std::sort(items.consumption.begin(), items.consumption.end(), byTotalDesc);

		return items;
	}
}

#include "production_icons.h"
#include "Plugin Core/Helpers/plugin_helpers.h"

#include "Chimera_classes.hpp"
#include "AuItems_classes.hpp"
#include "Engine_classes.hpp"
#include "AssetRegistry_classes.hpp"
#include "AssetRegistry_parameters.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace ProductionIcons
{
	namespace
	{
		struct IconEntry
		{
			SDK::UTexture2D*    icon            = nullptr; // UTexture2D from ItemIcon.ResourceObject
			bool                iconIsTexture2D = false;
			PluginTextureHandle handle          = nullptr;
		};

		// Populated on the game thread by RefreshIconsOnGameThread, adopted by
		// Tick() (also game thread, via OnEngineTick).
		std::mutex                                  g_pendingMutex;
		std::unordered_map<std::string, IconEntry>  g_pendingIcons;
		bool                                         g_pendingReady = false;
		std::atomic<bool>                            g_refreshInFlight{ false };

		// Owned by Tick()/GetIcon() - both called from the game thread.
		std::unordered_map<std::string, IconEntry> g_icons;
		bool                                        g_iconsLoaded = false;
		bool                                         g_needsRetry  = false; // some textures returned NULL (D3D12 not ready yet)

		bool HasMissingHandles(const std::unordered_map<std::string, IconEntry>& icons)
		{
			for (const auto& [key, entry] : icons)
				if (entry.iconIsTexture2D && entry.icon && !entry.handle)
					return true;
			return false;
		}

		void FreeIconTexHandles(std::unordered_map<std::string, IconEntry>& icons)
		{
			IPluginHooks* hooks = GetHooks();
			if (!hooks || !hooks->ImGuiTextures)
				return;

			for (auto& [key, entry] : icons)
			{
				if (entry.handle)
				{
					hooks->ImGuiTextures->FreeTexture(entry.handle);
					entry.handle = nullptr;
				}
			}
		}

		// Attempts to register UTexture2D icons with ImGui for every entry that
		// has no handle yet. Safe to call repeatedly - LoadFromUTexture2D returns
		// NULL (without throwing) when D3D12 isn't ready, so entries that miss
		// here are retried on the next call.
		void LoadIconTexHandles(std::unordered_map<std::string, IconEntry>& icons, IPluginSplash* splash = nullptr)
		{
			IPluginHooks* hooks = GetHooks();
			if (!hooks || !hooks->ImGuiTextures)
				return;

			const int total = static_cast<int>(icons.size());
			int loaded = 0;

			if (splash && splash->IsVisible())
				splash->SetSubStatus("(Production Viewer) Copying item icons... Wait!");

			for (auto& [key, entry] : icons)
			{
				if (entry.iconIsTexture2D && entry.icon && !entry.handle)
				{
					const std::string texName = entry.icon->GetName();
					entry.handle = hooks->ImGuiTextures->LoadFromUTexture2D(entry.icon, texName.c_str());
				}

				if (splash && splash->IsVisible() && total > 0)
				{
					++loaded;
					splash->SetSubProgress(static_cast<float>(loaded) / static_cast<float>(total));
				}
			}

			if (splash && splash->IsVisible())
				splash->ClearSubBar();
		}

		// ResourceObject might be UTexture, UMaterialInterface, etc. - the IsA
		// check at load time is the safe gate; just return it as UTexture2D* for
		// storage.
		SDK::UTexture2D* GetItemIconResource(SDK::UAuItemDataBase* item)
		{
			return item ? reinterpret_cast<SDK::UTexture2D*>(item->ItemIcon.ResourceObject) : nullptr;
		}

		// Every item type is defined as a Blueprint (UAuItemBlueprint asset whose
		// GeneratedClass's CDO is the actual UAuItemDataBase) - mirrors how the
		// game's own UCrUW_CheatItemsTab::DebugGatherAllItems enumerates the full
		// item list via the asset registry.
		SDK::FTopLevelAssetPath MakeItemBlueprintClassPath()
		{
			SDK::FString packagePath(L"/Script/AuItems");
			SDK::FString className(L"AuItemBlueprint");
			return SDK::UKismetSystemLibrary::MakeTopLevelAssetPath(packagePath, className);
		}

		SDK::UObject* CallGetAssetRegistry()
		{
			SDK::UAssetRegistryHelpers* cdo = SDK::UAssetRegistryHelpers::GetDefaultObj();
			if (!cdo)
				return nullptr;

			static SDK::UFunction* func = nullptr;
			if (!func)
				func = SDK::UAssetRegistryHelpers::StaticClass()->GetFunction("AssetRegistryHelpers", "GetAssetRegistry");
			if (!func)
			{
				LOG_WARN("ProductionIcons: could not resolve UAssetRegistryHelpers::GetAssetRegistry.");
				return nullptr;
			}

			SDK::Params::AssetRegistryHelpers_GetAssetRegistry parms{};
			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400;
			cdo->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;

			return parms.ReturnValue.GetObjectRef();
		}

		bool CallGetAssetsByClass(SDK::IAssetRegistry* registry, const SDK::FTopLevelAssetPath& classPath, SDK::TArray<SDK::FAssetData>& outAssets)
		{
			SDK::UObject* registryObject = registry ? registry->AsUObject() : nullptr;
			if (!registryObject)
				return false;

			static SDK::UFunction* func = nullptr;
			if (!func)
				func = SDK::IAssetRegistry::StaticClass()->GetFunction("AssetRegistry", "GetAssetsByClass");
			if (!func)
			{
				LOG_WARN("ProductionIcons: could not resolve IAssetRegistry::GetAssetsByClass.");
				return false;
			}

			SDK::Params::AssetRegistry_GetAssetsByClass parms{};
			parms.ClassPathName     = classPath;
			parms.bSearchSubClasses = true;

			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400;
			registryObject->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;

			outAssets = std::move(parms.OutAssetData);
			return parms.ReturnValue;
		}

		// Raw CoreUObject object/package lookup and loading functions - resolved
		// once via the AOB-scanned addresses the modloader exposes on
		// IPluginEngineEvents (same approach as BetterCheats' item spawner).
		using StaticFindObjectByNameFn = SDK::UObject*  (__fastcall*)(SDK::UClass*, SDK::UObject*, const wchar_t*, bool);
		using FindPackageFn            = SDK::UPackage* (__fastcall*)(SDK::UObject*, const wchar_t*);
		using PackageFullyLoadFn       = void           (__fastcall*)(SDK::UPackage*);
		using LoadPackageFn            = SDK::UPackage* (__fastcall*)(SDK::UPackage*, const wchar_t*, uint32_t, void*, const void*);

		StaticFindObjectByNameFn g_staticFindObjectByName = nullptr;
		FindPackageFn            g_findPackage             = nullptr;
		PackageFullyLoadFn       g_packageFullyLoad        = nullptr;
		LoadPackageFn            g_loadPackage             = nullptr;
		bool                     g_engineFnsResolveTried   = false;

		bool ResolveEngineLookupFunctions()
		{
			if (g_engineFnsResolveTried)
				return g_staticFindObjectByName && g_findPackage && g_packageFullyLoad && g_loadPackage;

			g_engineFnsResolveTried = true;

			IPluginHooks* hooks = GetHooks();
			IPluginEngineEvents* engine = hooks ? hooks->Engine : nullptr;
			if (!engine)
			{
				LOG_WARN("ProductionIcons: engine events interface unavailable - cannot resolve object/package lookup functions.");
				return false;
			}

			if (uintptr_t address = engine->GetStaticFindObjectByNameAddress())
				g_staticFindObjectByName = reinterpret_cast<StaticFindObjectByNameFn>(address);
			if (uintptr_t address = engine->GetFindPackageAddress())
				g_findPackage = reinterpret_cast<FindPackageFn>(address);
			if (uintptr_t address = engine->GetPackageFullyLoadAddress())
				g_packageFullyLoad = reinterpret_cast<PackageFullyLoadFn>(address);
			if (uintptr_t address = engine->GetLoadPackageAddress())
				g_loadPackage = reinterpret_cast<LoadPackageFn>(address);

			if (!g_staticFindObjectByName || !g_findPackage || !g_packageFullyLoad || !g_loadPackage)
			{
				LOG_WARN("ProductionIcons: failed to resolve object/package lookup functions - icon list will be incomplete.");
				return false;
			}

			return true;
		}

		// Forces the Blueprint's package to load and resolves it down to the
		// UAuItemDataBase CDO that actually holds the item's icon: package ->
		// generated "<AssetName>_C" class -> ClassDefaultObject.
		SDK::UAuItemDataBase* ResolveItemFromBlueprintAsset(const SDK::FAssetData& assetData)
		{
			if (!ResolveEngineLookupFunctions())
				return nullptr;

			const std::string assetName   = assetData.AssetName.ToString();
			const std::string packageName = assetData.PackageName.GetRawString();
			if (packageName.empty())
				return nullptr;

			const std::wstring packageNameW(packageName.begin(), packageName.end());
			const std::wstring generatedClassNameW = std::wstring(assetName.begin(), assetName.end()) + L"_C";

			SDK::UPackage* package = g_findPackage(nullptr, packageNameW.c_str());
			if (package)
				g_packageFullyLoad(package);
			else
				package = g_loadPackage(nullptr, packageNameW.c_str(), 0, nullptr, nullptr);

			if (!package)
				return nullptr;

			SDK::UObject* generatedObj = g_staticFindObjectByName(SDK::UClass::StaticClass(), package, generatedClassNameW.c_str(), false);
			if (!generatedObj)
				return nullptr;

			SDK::UClass* generatedClass = static_cast<SDK::UClass*>(generatedObj);
			SDK::UObject* cdo = generatedClass->ClassDefaultObject;
			if (!cdo || !cdo->IsA(SDK::UAuItemDataBase::StaticClass()))
				return nullptr;

			return static_cast<SDK::UAuItemDataBase*>(cdo);
		}

		// Enumerates every item type via the asset registry (mirrors
		// UCrUW_CheatItemsTab::DebugGatherAllItems / BetterCheats' item spawner),
		// resolves each one's icon, and pre-loads a GPU texture handle for it.
		// Runs on the game thread (posted via Init/Tick); publishes its result
		// through g_pendingIcons for Tick() to adopt.
		void RefreshIconsOnGameThread(void* /*context*/)
		{
			std::unordered_map<std::string, IconEntry> icons;

			IPluginSelf* self = GetSelf();
			IPluginSplash* splash = (self && self->hooks) ? self->hooks->Splash : nullptr;
			if (splash && splash->IsVisible())
			{
				splash->SetSubStatus("Production Viewer: scanning item icons...");
				splash->SetSubProgress(0.0f);
			}

			LOG_INFO("ProductionIcons: scanning the asset registry for item icons...");

			try
			{
				SDK::UObject* registryObject = CallGetAssetRegistry();
				SDK::IAssetRegistry* registry = registryObject ? reinterpret_cast<SDK::IAssetRegistry*>(registryObject) : nullptr;

				if (!registry)
				{
					LOG_WARN("ProductionIcons: could not resolve the asset registry.");
				}
				else
				{
					SDK::TArray<SDK::FAssetData> assetData;
					if (!CallGetAssetsByClass(registry, MakeItemBlueprintClassPath(), assetData))
						LOG_WARN("ProductionIcons: IAssetRegistry::GetAssetsByClass failed.");

					const int rawCount = assetData.Num();
					for (int i = 0; i < rawCount; ++i)
					{
						SDK::UAuItemDataBase* item = ResolveItemFromBlueprintAsset(assetData[i]);
						if (!item)
							continue;

						const std::string uniqueName = item->UniqueItemName.ToString();
						if (uniqueName.empty())
							continue;

						IconEntry entry;
						entry.icon = GetItemIconResource(item);
						if (entry.icon)
						{
							try { entry.iconIsTexture2D = entry.icon->IsA(SDK::UTexture2D::StaticClass()); }
							catch (...) {}
						}

						if (!entry.icon || !entry.iconIsTexture2D)
							continue;

						icons.emplace(uniqueName, std::move(entry));
					}

					LOG_INFO("ProductionIcons: resolved %d item icon(s) from %d Blueprint asset(s).",
						static_cast<int>(icons.size()), rawCount);
				}
			}
			catch (...)
			{
				LOG_DEBUG("ProductionIcons: exception while resolving item icons via the asset registry.");
			}

			// Pre-load texture handles while still on the game thread. D3D12 may
			// not be ready yet (returns NULL); Tick() retries on each call.
			LoadIconTexHandles(icons, splash);

			{
				std::lock_guard<std::mutex> lock(g_pendingMutex);
				g_pendingIcons = std::move(icons);
				g_pendingReady = true;
				g_refreshInFlight.store(false, std::memory_order_release);
			}

			if (splash)
			{
				splash->ClearSubBar();
				splash->ReleaseSplashHold();
			}
		}

		// Triggers an asynchronous icon-list refresh on the game thread. Safe to call
		// repeatedly (from Init or every Tick) - coalesces into a single in-flight
		// request via g_refreshInFlight. Mirrors BetterCheats' RequestRefreshItemList.
		void RequestRefresh()
		{
			bool expected = false;
			if (!g_refreshInFlight.compare_exchange_strong(expected, true))
				return;

			IPluginSelf* self = GetSelf();
			IPluginHooks* hooks = self ? self->hooks : nullptr;
			if (!hooks || !hooks->Engine || !hooks->ImGuiTextures)
			{
				g_refreshInFlight.store(false, std::memory_order_release);
				return;
			}

			hooks->Engine->PostToGameThread(&RefreshIconsOnGameThread, nullptr);
		}

		void AdoptPendingIconsIfReady()
		{
			std::unordered_map<std::string, IconEntry> incoming;
			{
				std::lock_guard<std::mutex> lock(g_pendingMutex);
				if (!g_pendingReady)
					return;

				incoming = std::move(g_pendingIcons);
				g_pendingIcons.clear();
				g_pendingReady = false;
			}

			FreeIconTexHandles(g_icons);
			g_icons       = std::move(incoming);
			g_iconsLoaded = true;
			g_needsRetry  = HasMissingHandles(g_icons);
			LOG_INFO("ProductionIcons: loaded %d item icon(s).", static_cast<int>(g_icons.size()));
		}
	}

	void Init(IPluginSelf* self)
	{
		IPluginSplash* splash = (self && self->hooks) ? self->hooks->Splash : nullptr;
		if (splash && splash->IsVisible())
			splash->AcquireSplashHold();

		IPluginHooks* hooks = self ? self->hooks : nullptr;
		if (!hooks || !hooks->Engine || !hooks->ImGuiTextures)
		{
			if (splash && splash->IsVisible())
				splash->ReleaseSplashHold();
			LOG_WARN("ProductionIcons: ImGui texture hooks unavailable (server/generic build) - icons disabled.");
			return;
		}

		RequestRefresh();
	}

	void Shutdown()
	{
		FreeIconTexHandles(g_icons);
		g_icons.clear();
		g_iconsLoaded = false;
	}

	void Tick()
	{
		AdoptPendingIconsIfReady();

		// Initial scan hasn't completed yet (e.g. the Init-time post never ran) -
		// keep retrying every tick until it succeeds, mirroring BetterCheats'
		// "if (!g_itemsLoaded) RequestRefreshItemList();" retry-from-render pattern.
		if (!g_iconsLoaded)
		{
			RequestRefresh();
			return;
		}

		// Retry any entries whose texture wasn't ready on the previous attempt.
		if (g_needsRetry)
		{
			LoadIconTexHandles(g_icons);
			g_needsRetry = HasMissingHandles(g_icons);
		}
	}

	PluginTextureHandle GetIcon(const std::string& uniqueItemName)
	{
		auto it = g_icons.find(uniqueItemName);
		return it != g_icons.end() ? it->second.handle : nullptr;
	}
}

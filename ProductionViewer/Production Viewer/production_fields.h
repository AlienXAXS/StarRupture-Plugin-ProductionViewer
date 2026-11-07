#pragma once

#include "plugin_interface.h"
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"
#include "MassCommon_structs.hpp"
#include "MassSignals_classes.hpp"
#include "AuItems_structs.hpp"
#include "plugin_map.h"

#include <cstdint>
#include <iterator>
#include <vector>

// Every member of a game class the production viewer reads, taken from the
// loader's map for the running game build (see plugin_map.h). Resolved once in
// OnPluginLoadHooks; read on the game thread with In(object), which is null
// when the object is null or the member did not resolve. Structs that are only
// stepped through (an array's elements, a stack copy handed to the game) are
// covered by a size check. Engine members (UObject, UWorld, FVector, the
// refcount of a TSharedPtr controller) stay compiled in.

#define PRODUCTIONVIEWER_FIELD(Class, Member) SR_FIELD(decltype(SDK::Class::Member), Class, Member, 0)

namespace ProductionViewerFields
{
	inline auto ItemUniqueName = PRODUCTIONVIEWER_FIELD(UAuItemDataBase, UniqueItemName);
	inline auto ItemName       = PRODUCTIONVIEWER_FIELD(UAuItemDataBase, ItemName);
	inline auto ItemIcon       = PRODUCTIONVIEWER_FIELD(UAuItemDataBase, ItemIcon);

	inline auto CraftingSelectedRecipe = PRODUCTIONVIEWER_FIELD(FCrCraftingFragment, SelectedRecipe);
	inline auto CraftingCurrentRecipe  = PRODUCTIONVIEWER_FIELD(FCrCraftingFragment, CurrentRecipe);
	inline auto CraftingMultiplier     = PRODUCTIONVIEWER_FIELD(FCrCraftingFragment, CraftingMultiplier);

	// An item stack, read off a recipe's output and inputs.
	inline auto ItemStackData  = PRODUCTIONVIEWER_FIELD(FAuSimpleItem, ItemDataBase);
	inline auto ItemStackCount = PRODUCTIONVIEWER_FIELD(FAuSimpleItem, Count);
	inline auto ItemStackSize  = SR_SIZE_CHECK(FAuSimpleItem);

	// A stack copy of the helper is handed to the game's constructor, so the
	// whole struct is checked against the running build.
	inline auto ReplicationHelperSize = SR_SIZE_CHECK(FCrMassEntityReplicationHelper);

	// The transform a Mass entity's FTransformFragment holds. Reflected. The
	// entity manager and the signal subsystem's world come from the SDK's Mass
	// helpers (PluginMap::MassEntityManagerField, MassSignalWorldField()).
	inline auto TransformFragmentTransform = PRODUCTIONVIEWER_FIELD(FTransformFragment, Transform);

	using PluginMap::SharedRef;

	// FCrGridEntityHandle's two members sit in a struct the SDK keeps as
	// padding. Their rows below are CL-127004's, the build this plugin was
	// compiled against; every other build comes from the loader's map, and a
	// build with neither turns off the feature using them.
	inline auto GridEntityHandle = PluginField<uint64_t>{ { "FCrGridEntityHandle::EntityHandle",
		"FCrGridEntityHandle", "EntityHandle", PLUGIN_FIELD_NATIVE, sizeof(uint64_t), 0x0 } };
	inline auto GridEntityData = PluginField<SharedRef>{ { "FCrGridEntityHandle::Data",
		"FCrGridEntityHandle", "Data", PLUGIN_FIELD_NATIVE, sizeof(SharedRef), 0x8 } };
	// Stepped through as an array element: the size the running game reflects,
	// read with PluginMap::ElementAt.
	inline auto GridEntityHandleSize = SR_SIZE(FCrGridEntityHandle);

	// An FTransform's translation. The engine's struct keeps it in a SIMD
	// register, 0x20 bytes in the PDB, while the reflected and SDK member is the
	// 0x18-byte FVector, so it is a native row at the size the plugin reads.
	inline auto TransformTranslation = PluginField<SDK::FVector>{ { "FTransform::Translation",
		"FTransform", "Translation", PLUGIN_FIELD_NATIVE, sizeof(SDK::FVector), 0x20 } };

	inline const PluginFieldCandidate kGridEntityHandleRows[] = {
		{ 127004, 0x0, static_cast<uint32_t>(sizeof(uint64_t)), 0, nullptr },
	};
	inline const PluginFieldCandidate kGridEntityDataRows[] = {
		{ 127004, 0x8, static_cast<uint32_t>(sizeof(SharedRef)), 0, nullptr },
	};
	inline const PluginFieldCandidate kTransformTranslationRows[] = {
		{ 127004, 0x20, static_cast<uint32_t>(sizeof(SDK::FVector)), 0, nullptr },
	};

	// UCrGameInstance::ServerSessionName is folded into the SDK's trailing padding,
	// so it has no name there: a native row. The row below is CL-127004's, the
	// build this plugin was compiled against; every other build comes from the
	// loader's map.
	inline auto ServerSessionName = PluginField<SDK::FString>{ { "UCrGameInstance::ServerSessionName",
		"UCrGameInstance", "ServerSessionName", PLUGIN_FIELD_NATIVE, sizeof(SDK::FString), 0x290 } };

	inline const PluginFieldCandidate kServerSessionNameRows[] = {
		{ 127004, 0x290, sizeof(SDK::FString), 0, nullptr },
	};

	// The SDK's helpers, under the names the plugin uses.
	using PluginMap::KeysOf;
	using PluginMap::ValueOr;

	// The parts of the plugin that can be off while the rest runs. A key the
	// loader cannot resolve on a game build turns off the part that needs it;
	// the others carry on. A key two parts need is listed in both: base cores
	// read an entity's location through the same subsystem and fragment
	// accessors as tracking. The viewer window and the network stay core.
	inline PluginMap::Feature Tracking{ "tracking", "Counting what factories craft",
		KeysOf("UWorld::GetSubsystem<UMassEntitySubsystem>", "FCrCraftingFragment::StaticStruct",
			"FMassEntityManager::InternalGetFragmentDataPtr",
			"UMassSignalSubsystem::SignalEntity",
			CraftingSelectedRecipe, CraftingCurrentRecipe, CraftingMultiplier,
			ItemUniqueName, ItemName, ItemStackData, ItemStackCount, ItemStackSize,
			PluginMap::MassEntityManagerField, PluginMap::MassSignalWorldField()), {} };
	inline PluginMap::Feature BaseCores{ "basecores", "Splitting production by base",
		KeysOf("UCrBaseCoreSubsystem::GetBaseCoresRangedBuilding",
			"UWorld::GetSubsystem<UMassEntitySubsystem>", "FCrCraftingFragment::StaticStruct",
			"FMassEntityManager::InternalGetFragmentDataPtr",
			"FCrMassEntityReplicationHelper::FCrMassEntityReplicationHelper",
			"UCrBuildingCustomNameSubsystem::GetBuildingCustomNameByReplicationHelper",
			ReplicationHelperSize, GridEntityHandle, GridEntityData, GridEntityHandleSize,
			PluginMap::MassEntityManagerField, TransformFragmentTransform, TransformTranslation), {} };
	inline PluginMap::Feature ItemIcons{ "icons", "Item icons", KeysOf(ItemIcon, ItemUniqueName), {} };
	inline PluginMap::Feature SessionSave{ "sessionsave", "Keeping totals for each saved game",
		KeysOf(ServerSessionName), {} };

	// Call before Resolve and ProductionTracker::ResolvePatterns, so a miss
	// lands on a feature and not on the plugin.
	inline void DeclareFeatures(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		PluginMap::DeclareFeatures(scanner, self, Tracking, BaseCores, ItemIcons, SessionSave);
	}

	// Asks the loader for every row above. False when any did not resolve; the
	// loader has then recorded the failure and refuses the plugin.
	inline bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		bool all = PluginMap::ResolveFields(scanner, self,
			ItemUniqueName, ItemName, ItemIcon,
			CraftingSelectedRecipe, CraftingCurrentRecipe, CraftingMultiplier,
			ItemStackData, ItemStackCount, ItemStackSize, ReplicationHelperSize,
			TransformFragmentTransform, GridEntityHandleSize);
		all = PluginMap::ResolveMassSignalWorld(scanner, self) && all;
		all = PluginMap::ResolveMassEntityManager(scanner, self) && all;
		all = PluginMap::ResolveField(scanner, self, ServerSessionName, kServerSessionNameRows,
			static_cast<uint32_t>(std::size(kServerSessionNameRows))) && all;
		all = PluginMap::ResolveField(scanner, self, GridEntityHandle, kGridEntityHandleRows,
			static_cast<uint32_t>(std::size(kGridEntityHandleRows))) && all;
		all = PluginMap::ResolveField(scanner, self, GridEntityData, kGridEntityDataRows,
			static_cast<uint32_t>(std::size(kGridEntityDataRows))) && all;
		all = PluginMap::ResolveField(scanner, self, TransformTranslation, kTransformTranslationRows,
			static_cast<uint32_t>(std::size(kTransformTranslationRows))) && all;
		return all;
	}
}

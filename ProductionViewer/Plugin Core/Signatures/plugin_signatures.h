#pragma once

#include <cstdint>

// Central registry of AOB patterns and fixed image-base RVAs used to locate
// game engine functions/data at runtime. When a game update breaks one of
// these, this is the only file that should need editing - assuming the
// underlying function's shape is unchanged, in which case the dependent
// follow-call offsets (also listed here) may need recomputing too.
namespace Signatures
{
	// ---- ProductionTracker (production_tracker.cpp) ----

	// UCrCraftingComponent::GetCraftingFragment(UCrCraftingComponent*)
	// Returns the live FCrCraftingFragment* holding CurrentRecipe/SelectedRecipe.
	// Used as a fallback when ItemsToCraft has already been drained by the time
	// OnCraftingFinished fires (mirrors WAILA's crafter_detector).
	inline constexpr const char* GetCraftingFragment =
		"48 89 5C 24 ?? 57 48 83 EC ?? 48 8B 99 ?? ?? ?? ?? ?? ?? ?? 48 8B B8 ?? ?? ?? ?? "
		"E8 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? ?? 48 89 44 24 ?? 74 ?? 48 85 C0 74 ?? 48 8B C8 "
		"E8 ?? ?? ?? ?? 48 8D 54 24 ?? 48 8B CB FF D7 48 85 C0 74 ?? 48 8B C8 E8 ?? ?? ?? ?? "
		"48 85 C0 75 ?? 33 C0 48 8B 5C 24 ?? 48 83 C4 ?? 5F C3 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 83 EC";

	// ---- ProductionMass (production_mass.cpp) ----

	// UCrMassActorComponent::GetMassFragment<FCrCraftingFragment> - xref AOB.
	// Matches a call site that calls GetMassFragment<FCrCraftingFragment>; the
	// E8 rel32 at offset 0 is followed at runtime to reach the function entry.
	// GetMassFragment_FCrCraftingFragment_EntryOffset is set to the sentinel
	// value ~0 to signal that the match address is a call site, not the entry.
	inline constexpr const char* GetMassFragment_FCrCraftingFragment =
		"E8 ?? ?? ?? ?? 48 85 C0 75 ?? 33 C0 48 8B 5C 24 ?? 48 83 C4 ?? 5F C3 "
		"?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 83 EC";

	// Sentinel: the AOB matches a call site. ResolveFragmentAccessors follows
	// the E8 rel32 at the match address to get the actual function entry.
	inline constexpr uintptr_t GetMassFragment_FCrCraftingFragment_EntryOffset = ~0ULL;

	// Offsets of the `call`/`jmp rel32` instructions inside
	// GetMassFragment<FCrCraftingFragment> (relative to its entry point, i.e.
	// addr + GetMassFragment_FCrCraftingFragment_EntryOffset) that are
	// followed to resolve FCrCraftingFragment::StaticStruct (call at +0x55)
	// and FMassEntityManager::InternalGetFragmentDataPtr (tail-jmp at +0x6D).
	inline constexpr uintptr_t GetMassFragment_StaticStructCallOffset       = 0x55;
	inline constexpr uintptr_t GetMassFragment_GetFragmentDataPtrCallOffset = 0x6D;

	// UMassSignalSubsystem::SignalEntity(UMassSignalSubsystem*, FName, FMassEntityHandle)
	// Called for every Mass entity (loaded or simulated) whenever a signal fires,
	// including CrMassSignals::CraftingItemComplete on craft completion.
	inline constexpr const char* MassSignalSubsystem_SignalEntity =
		"48 89 5C 24 ?? 4C 89 44 24 ?? 57 48 83 EC ?? 48 8B DA 48 8B F9 45 85 C0";

	// UWorld::GetSubsystem<UMassEntitySubsystem>() - returns the cached
	// UMassEntitySubsystem* for a UWorld (UWorld* in RCX, __fastcall).
	inline constexpr const char* GetMassEntitySubsystem =
		"48 89 5C 24 ?? 57 48 83 EC ?? 48 8D B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? ?? 48 8B D8 74 ?? "
		"48 85 C0 74 ?? 48 8B C8 E8 ?? ?? ?? ?? EB ?? 48 85 DB 74 ?? E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8D 50 ?? "
		"48 63 40 ?? 3B 43 ?? 7F ?? 48 8B C8 48 8B 43 ?? ?? ?? ?? ?? 74 ?? 33 DB 48 8B D3 48 8B CF 48 8B 5C 24 ?? "
		"48 83 C4 ?? 5F E9 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 4C 89 4C 24 ?? 4C 89 44 24";

}

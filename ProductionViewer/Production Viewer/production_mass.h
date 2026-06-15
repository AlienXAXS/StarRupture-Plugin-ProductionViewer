#pragma once

#include "plugin_interface.h"

namespace SDK
{
	class UMassSignalSubsystem;
	struct FCrCraftingFragment;
}

// Catches crafting-complete events for *every* Mass crafting entity, including
// ones whose actor is currently de-spawned/simulated (i.e. far from the
// player). This is the Mass-ECS analog of IPluginCraftingEvents::
// RegisterOnCraftingFinished, which only fires for entities with a loaded
// ACrCrafter actor.
//
// Implementation: hooks UMassSignalSubsystem::SignalEntity (called by
// UCrCraftingProcessor for every entity, loaded or not, when its crafting
// completes) and filters for the CrMassSignals::CraftingItemComplete signal.
// On a match, the entity's FCrCraftingFragment is read directly from the
// world's FMassEntityManager via FMassEntityManager::InternalGetFragmentDataPtr.
namespace ProductionMass
{
	// Mirrors the engine's FMassEntityHandle (MassEntityTypes.h) - 8 bytes,
	// passed by value in a single register under the x64 calling convention.
	struct FMassEntityHandle
	{
		uint32_t Index;
		uint32_t SerialNumber;
	};

	// Invoked with the live FCrCraftingFragment for any Mass entity (loaded or
	// simulated) whose crafting just completed. The fragment pointer is only
	// valid for the duration of the call.
	using CraftingCompleteCallback = void (*)(const SDK::FCrCraftingFragment* fragment);

	// AOB-resolves the helper functions and installs the SignalEntity hook.
	// Returns false (and logs a warning) if any required pattern could not be
	// resolved; in that case Mass-simulated crafting will not be tracked, but
	// the actor-hook path keeps working.
	bool Init(IPluginSelf* self, CraftingCompleteCallback callback);

	// Removes the SignalEntity hook installed by Init.
	void Shutdown(IPluginSelf* self);
}

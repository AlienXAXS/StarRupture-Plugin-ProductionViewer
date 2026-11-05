#include "production_mass.h"
#include "Plugin Core/Helpers/plugin_helpers.h"
#include "Plugin Core/Signatures/plugin_signatures.h"

#include "Basic.hpp"
#include "CoreUObject_classes.hpp"
#include "Chimera_structs.hpp"
#include "MassSignals_classes.hpp"
#include "plugin_map.h"
#include "production_fields.h"

#include <cstdio>
#include <string>

namespace ProductionMass
{
	namespace
	{
		using StaticStructFn       = SDK::UScriptStruct* (*)();
		using GetFragmentDataPtrFn = void* (*)(void* entityManager, FMassEntityHandle entity, const SDK::UScriptStruct* fragmentType);
		using SignalEntityFn       = void (__fastcall*)(SDK::UMassSignalSubsystem*, SDK::FName, FMassEntityHandle);

		using GetMassEntitySubsystemFn = void* (__fastcall*)(SDK::UWorld*);

		StaticStructFn       g_craftingFragmentStaticStruct = nullptr;
		GetFragmentDataPtrFn g_getFragmentDataPtr = nullptr;
		GetMassEntitySubsystemFn g_getMassEntitySubsystem = nullptr;
		SignalEntityFn       g_originalSignalEntity = nullptr;
		HookHandle           g_signalEntityHook = nullptr;
		CraftingCompleteCallback g_callback = nullptr;

		// Logs the first 16 bytes at `ptr`, a function address the loader resolved.
		void LogCodeBytes(const char* label, const void* ptr)
		{
			if (!ptr)
			{
				LOG_DEBUG("ProductionMass: %s = null", label);
				return;
			}

			const auto* bytes = reinterpret_cast<const uint8_t*>(ptr);
			char hex[3 * 16 + 1] = {};
			for (int i = 0; i < 16; ++i)
				std::snprintf(hex + i * 3, 4, "%02X ", bytes[i]);
			LOG_DEBUG("ProductionMass: %s = %p [%s]", label, ptr, hex);
		}

		// Returns the FMassEntityManager backing `world`'s Mass simulation, or
		// nullptr if the Mass entity subsystem isn't available.
		void* ResolveEntityManager(SDK::UWorld* world)
		{
			void* subsystem = g_getMassEntitySubsystem(world);
			if (!subsystem)
				return nullptr;

			// UMassEntitySubsystem::EntityManager is a TSharedPtr<FMassEntityManager>;
			// the raw FMassEntityManager* is its .Object. Null when the loader has no
			// row for the member on this build.
			return PluginMap::MassEntityManagerOf(subsystem);
		}

		// UWorld::GetSubsystem<UMassEntitySubsystem>()
		bool ResolveGetMassEntitySubsystem(IPluginSelf* self, IPluginHookScanner* scanner)
		{
			// FUNCTION_START: called with a UWorld* in RCX.
			uintptr_t addr = PluginMap::Function(scanner, self, "UWorld::GetSubsystem<UMassEntitySubsystem>",
				Signatures::GetMassEntitySubsystem, PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);
			if (!addr)
			{
				LOG_WARN("ProductionMass: UWorld::GetSubsystem<UMassEntitySubsystem> unresolved");
				return false;
			}

			LogCodeBytes("GetMassEntitySubsystem", reinterpret_cast<void*>(addr));

			g_getMassEntitySubsystem = reinterpret_cast<GetMassEntitySubsystemFn>(addr);
			return true;
		}

		void __fastcall HookedSignalEntity(SDK::UMassSignalSubsystem* self, SDK::FName signalName, FMassEntityHandle entity)
		{
			if (g_originalSignalEntity)
				g_originalSignalEntity(self, signalName, entity);

			if (!g_callback || !self)
				return;

			if (signalName.ToString() != "CraftingItemComplete")
				return;

			LOG_DEBUG("ProductionMass: SignalEntity - CraftingItemComplete for entity Index=%u SerialNumber=%u",
				entity.Index, entity.SerialNumber);

			SDK::UWorld* world = PluginMap::MassSignalWorldOf(self);
			if (!world)
			{
				LOG_DEBUG("ProductionMass: SignalEntity - CachedWorld is null, ignoring");
				return;
			}

			void* entityManager = ResolveEntityManager(world);
			if (!entityManager)
			{
				LOG_DEBUG("ProductionMass: SignalEntity - failed to resolve FMassEntityManager for world=%p, ignoring", world);
				return;
			}

			SDK::UScriptStruct* fragmentType = g_craftingFragmentStaticStruct();
			LOG_DEBUG("ProductionMass: SignalEntity - world=%p entityManager=%p fragmentType=%p (%s)",
				world, entityManager, fragmentType,
				fragmentType ? fragmentType->GetName().c_str() : "null");

			auto* fragment = static_cast<const SDK::FCrCraftingFragment*>(g_getFragmentDataPtr(entityManager, entity, fragmentType));
			LOG_DEBUG("ProductionMass: SignalEntity - fragment=%p", fragment);

			if (fragment)
				g_callback(fragment, world, entity);
		}

		// FCrCraftingFragment::StaticStruct and
		// FMassEntityManager::InternalGetFragmentDataPtr, each asked for by its own
		// key. Both patterns match a call site (see plugin_signatures.h), so both
		// ask the loader to follow the E8 rel32 and judge the function it lands on.
		bool ResolveFragmentAccessors(IPluginSelf* self, IPluginHookScanner* scanner)
		{
			const uintptr_t staticStructAddr = PluginMap::Function(scanner, self, "FCrCraftingFragment::StaticStruct",
				Signatures::CraftingFragmentStaticStruct, PLUGIN_SCAN_FUNCTION_START,
				PLUGIN_SCAN_FLAG_OPTIONAL | PLUGIN_SCAN_FLAG_FOLLOW_REL32, 0,
				Signatures::CraftingFragmentStaticStructFollowAt);
			const uintptr_t getFragmentDataPtrAddr = PluginMap::Function(scanner, self,
				"FMassEntityManager::InternalGetFragmentDataPtr",
				Signatures::GetFragmentDataPtr, PLUGIN_SCAN_FUNCTION_START,
				PLUGIN_SCAN_FLAG_OPTIONAL | PLUGIN_SCAN_FLAG_FOLLOW_REL32, 0,
				Signatures::GetFragmentDataPtrFollowAt);
			if (!staticStructAddr || !getFragmentDataPtrAddr)
			{
				LOG_WARN("ProductionMass: ResolveFragmentAccessors - StaticStruct=0x%llX or "
					"GetFragmentDataPtr=0x%llX unresolved",
					static_cast<unsigned long long>(staticStructAddr), static_cast<unsigned long long>(getFragmentDataPtrAddr));
				return false;
			}

			LogCodeBytes("StaticStruct target", reinterpret_cast<void*>(staticStructAddr));
			LogCodeBytes("GetFragmentDataPtr target", reinterpret_cast<void*>(getFragmentDataPtrAddr));

			g_craftingFragmentStaticStruct = reinterpret_cast<StaticStructFn>(staticStructAddr);
			g_getFragmentDataPtr = reinterpret_cast<GetFragmentDataPtrFn>(getFragmentDataPtrAddr);
			return true;
		}

		// UMassSignalSubsystem::SignalEntity(UMassSignalSubsystem*, FName, FMassEntityHandle)
		// Called for every Mass entity (loaded or simulated) whenever a signal fires,
		// including CrMassSignals::CraftingItemComplete on craft completion.
		uintptr_t ResolveSignalEntity(IPluginSelf* self, IPluginHookScanner* scanner)
		{
			// FUNCTION_START: detoured.
			uintptr_t addr = PluginMap::Function(scanner, self, "UMassSignalSubsystem::SignalEntity",
				Signatures::MassSignalSubsystem_SignalEntity, PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);
			if (!addr)
				LOG_WARN("ProductionMass: UMassSignalSubsystem::SignalEntity unresolved");

			return addr;
		}

		// Resolved during OnPluginLoadHooks; 0 means the pattern missed on this build.
		uintptr_t g_signalEntityAddr = 0;
	}

	bool GetEntityLocation(SDK::UWorld* world, FMassEntityHandle entity, SDK::FVector& outLocation)
	{
		if (!world || !g_getFragmentDataPtr || !g_getMassEntitySubsystem)
		{
			LOG_TRACE("ProductionMass: GetEntityLocation - unavailable (world=%p getFragment=%p getSubsystem=%p)",
				static_cast<void*>(world), reinterpret_cast<void*>(g_getFragmentDataPtr),
				reinterpret_cast<void*>(g_getMassEntitySubsystem));
			return false;
		}

		void* entityManager = ResolveEntityManager(world);
		if (!entityManager)
		{
			LOG_DEBUG("ProductionMass: GetEntityLocation - failed to resolve FMassEntityManager for world=%p",
				static_cast<void*>(world));
			return false;
		}

		// FTransformFragment is a stock engine Mass fragment, so its
		// UScriptStruct is reachable through reflection - unlike
		// FCrCraftingFragment, whose StaticStruct had to be AOB-resolved.
		// Resolved once and cached; the Mass module is guaranteed loaded by
		// the time any entity lookup makes sense.
		static SDK::UScriptStruct* s_transformFragmentStruct = nullptr;
		if (!s_transformFragmentStruct)
		{
			s_transformFragmentStruct = SDK::UObject::FindObjectFast<SDK::UScriptStruct>("TransformFragment");
			if (!s_transformFragmentStruct)
			{
				LOG_WARN("ProductionMass: GetEntityLocation - ScriptStruct 'TransformFragment' not found");
				return false;
			}
			LOG_DEBUG("ProductionMass: GetEntityLocation - resolved TransformFragment struct %p (%s)",
				static_cast<void*>(s_transformFragmentStruct), s_transformFragmentStruct->GetFullName().c_str());
		}

		const void* fragment = g_getFragmentDataPtr(entityManager, entity, s_transformFragmentStruct);
		if (!fragment)
		{
			LOG_DEBUG("ProductionMass: GetEntityLocation - entity Index=%u Serial=%u has no FTransformFragment",
				entity.Index, entity.SerialNumber);
			return false;
		}

		const SDK::FVector* translation = ProductionViewerFields::TransformTranslation.In(
			ProductionViewerFields::TransformFragmentTransform.In(fragment));
		if (!translation)
		{
			LOG_DEBUG("ProductionMass: GetEntityLocation - transform members unresolved on this build");
			return false;
		}

		outLocation = *translation;
		return true;
	}

	void ResolvePatterns(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		if (!self || !scanner)
			return;

		// Optional throughout: a miss costs Mass-simulated crafting tracking but
		// leaves the actor-based tracker working, which is what the old
		// scan-at-init path did.
		if (!ResolveGetMassEntitySubsystem(self, scanner))
			return;

		if (!ResolveFragmentAccessors(self, scanner))
			return;

		g_signalEntityAddr = ResolveSignalEntity(self, scanner);
	}

	bool Init(IPluginSelf* self, CraftingCompleteCallback callback)
	{
		if (!ProductionViewerFields::Tracking.On())
		{
			LOG_WARN("ProductionMass: crafting counts are off - the game update moved something they need");
			return false;
		}

		if (!self->hooks->Hooks)
		{
			LOG_WARN("ProductionMass: hooks unavailable - Mass-simulated crafting will not be tracked");
			return false;
		}

		if (!g_getMassEntitySubsystem || !g_getFragmentDataPtr)
		{
			LOG_WARN("ProductionMass: Mass accessors unresolved - Mass-simulated crafting will not be tracked");
			return false;
		}

		const uintptr_t signalEntityAddr = g_signalEntityAddr;
		if (!signalEntityAddr)
			return false;

		g_callback = callback;
		g_signalEntityHook = self->hooks->Hooks->Install(signalEntityAddr, reinterpret_cast<void*>(&HookedSignalEntity), reinterpret_cast<void**>(&g_originalSignalEntity));
		if (!g_signalEntityHook)
		{
			LOG_WARN("ProductionMass: failed to install SignalEntity hook");
			g_callback = nullptr;
			return false;
		}

		LOG_INFO("ProductionMass: SignalEntity hook installed - Mass-simulated crafting will be tracked");
		return true;
	}

	void Shutdown(IPluginSelf* self)
	{
		if (g_signalEntityHook && self->hooks->Hooks)
		{
			self->hooks->Hooks->Remove(g_signalEntityHook);
			g_signalEntityHook = nullptr;
		}

		g_originalSignalEntity = nullptr;
		g_callback = nullptr;
	}
}

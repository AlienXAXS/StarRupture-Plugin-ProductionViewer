#include "production_mass.h"
#include "Plugin Core/Helpers/plugin_helpers.h"
#include "Plugin Core/Signatures/plugin_signatures.h"

#include "Basic.hpp"
#include "CoreUObject_classes.hpp"
#include "Chimera_structs.hpp"
#include "MassSignals_classes.hpp"

#include <windows.h>
#include <cstdio>
#include <cstring>
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

		// Reads the 32-bit displacement of a `call`/`jmp rel32` instruction
		// (opcode E8/E9, 5 bytes total) at `instrAddr` and returns its target.
		uintptr_t FollowRelCall(uintptr_t instrAddr)
		{
			int32_t rel = 0;
			std::memcpy(&rel, reinterpret_cast<const void*>(instrAddr + 1), sizeof(rel));
			return instrAddr + 5 + static_cast<uintptr_t>(rel);
		}

		// Returns the [base, base+size) address range of the main module, or
		// {0, 0} if it can't be determined.
		void GetMainModuleRange(uintptr_t& base, uintptr_t& size)
		{
			HMODULE mod = GetModuleHandle(nullptr);
			base = reinterpret_cast<uintptr_t>(mod);
			size = 0;
			if (!mod)
				return;

			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(mod) + dos->e_lfanew);
			size = nt->OptionalHeader.SizeOfImage;
		}

		// Sanity check used before treating a FollowRelCall result as a callable
		// function pointer - guards against AOB/offset drift sending us into a
		// garbage address (which can corrupt the stack and hard-crash the game
		// rather than cleanly access-violate).
		bool LooksLikeCodePointer(const void* ptr)
		{
			uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
			uintptr_t base = 0, size = 0;
			GetMainModuleRange(base, size);
			return base != 0 && addr >= base && addr < base + size;
		}

		// Logs the first 16 bytes at `ptr` for offset-tuning diagnostics. Only
		// call this after LooksLikeCodePointer(ptr) has returned true.
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

		// Logs `length` bytes starting at `ptr` in 16-byte rows, each prefixed
		// with its offset from `ptr`. Diagnostic-only - used to manually locate
		// call/jmp instructions when offset constants drift after a game update.
		void LogCodeRange(const char* label, const void* ptr, size_t length)
		{
			if (!ptr)
			{
				LOG_DEBUG("ProductionMass: %s = null", label);
				return;
			}

			const auto* bytes = reinterpret_cast<const uint8_t*>(ptr);
			for (size_t row = 0; row < length; row += 16)
			{
				char hex[3 * 16 + 1] = {};
				size_t rowLen = (length - row) < 16 ? (length - row) : 16;
				for (size_t i = 0; i < rowLen; ++i)
					std::snprintf(hex + i * 3, 4, "%02X ", bytes[row + i]);
				LOG_DEBUG("ProductionMass: %s +0x%02zX: %s", label, row, hex);
			}
		}

		// Returns the FMassEntityManager backing `world`'s Mass simulation, or
		// nullptr if the Mass entity subsystem isn't available.
		void* ResolveEntityManager(SDK::UWorld* world)
		{
			void* subsystem = g_getMassEntitySubsystem(world);
			if (!subsystem)
				return nullptr;

			// UMassEntitySubsystem::EntityManager (TSharedPtr<FMassEntityManager>) at
			// offset 0x38 - .Object is the raw FMassEntityManager* (MassEntity_classes.hpp,
			// Pad_38[0x10]).
			return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(subsystem) + 0x38);
		}

		// UWorld::GetSubsystem<UMassEntitySubsystem>()
		bool ResolveGetMassEntitySubsystem()
		{
			IPluginScanner* scanner = GetScanner();
			if (!scanner)
				return false;

			uintptr_t addr = scanner->FindPatternInMainModule(Signatures::GetMassEntitySubsystem);
			if (!addr)
			{
				LOG_WARN("ProductionMass: UWorld::GetSubsystem<UMassEntitySubsystem> pattern not found");
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

			SDK::UWorld* world = *reinterpret_cast<SDK::UWorld**>(reinterpret_cast<uint8_t*>(self) + 0xB0);
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
			{
				const auto* bytes = reinterpret_cast<const uint8_t*>(fragment);
				char hex[3 * 0x20 + 1] = {};
				for (int i = 0; i < 0x20; ++i)
					std::snprintf(hex + i * 3, 4, "%02X ", bytes[i]);
				LOG_DEBUG("ProductionMass: SignalEntity - fragment[0x00-0x1F]: %s", hex);

				for (int i = 0; i < 0x20; ++i)
					std::snprintf(hex + i * 3, 4, "%02X ", bytes[0x20 + i]);
				LOG_DEBUG("ProductionMass: SignalEntity - fragment[0x20-0x3F]: %s", hex);

				for (int i = 0; i < 0x20; ++i)
					std::snprintf(hex + i * 3, 4, "%02X ", bytes[0x60 + i]);
				LOG_DEBUG("ProductionMass: SignalEntity - fragment[0x60-0x7F]: %s", hex);
			}

			if (fragment)
				g_callback(fragment);
		}

		// UCrMassActorComponent::GetMassFragment<FCrCraftingFragment> - see the
		// comment on Signatures::GetMassFragment_FCrCraftingFragment. The AOB
		// matches a call site; the E8 rel32 at the match address is followed to
		// reach the function entry. From there, see
		// Signatures::GetMassFragment_StaticStructCallOffset/
		// GetMassFragment_GetFragmentDataPtrCallOffset for the call/jmp offsets.
		bool ResolveFragmentAccessors()
		{
			IPluginScanner* scanner = GetScanner();
			if (!scanner)
				return false;

			if (!*Signatures::GetMassFragment_FCrCraftingFragment)
			{
				LOG_WARN("ProductionMass: GetMassFragment<FCrCraftingFragment> pattern not set");
				return false;
			}

			uintptr_t addr = scanner->FindPatternInMainModule(Signatures::GetMassFragment_FCrCraftingFragment);
			if (!addr)
			{
				LOG_WARN("ProductionMass: GetMassFragment<FCrCraftingFragment> pattern not found");
				return false;
			}

			uintptr_t fragmentFn;
			if (Signatures::GetMassFragment_FCrCraftingFragment_EntryOffset == ~0ULL)
			{
				// Xref AOB: the match is a call site; follow the E8 rel32 to the entry.
				fragmentFn = FollowRelCall(addr);
				if (!fragmentFn)
				{
					LOG_WARN("ProductionMass: ResolveFragmentAccessors - xref AOB call follow failed");
					return false;
				}
			}
			else
			{
				fragmentFn = addr + Signatures::GetMassFragment_FCrCraftingFragment_EntryOffset;
			}

			LOG_DEBUG("ProductionMass: ResolveFragmentAccessors - AOB match=0x%llX fragmentFn=0x%llX",
				static_cast<unsigned long long>(addr), static_cast<unsigned long long>(fragmentFn));
			LogCodeRange("fragmentFn", reinterpret_cast<void*>(fragmentFn), 0x90);

			uintptr_t staticStructAddr = FollowRelCall(fragmentFn + Signatures::GetMassFragment_StaticStructCallOffset);
			uintptr_t getFragmentDataPtrAddr = FollowRelCall(fragmentFn + Signatures::GetMassFragment_GetFragmentDataPtrCallOffset);

			if (!LooksLikeCodePointer(reinterpret_cast<void*>(staticStructAddr)) ||
				!LooksLikeCodePointer(reinterpret_cast<void*>(getFragmentDataPtrAddr)))
			{
				LOG_WARN("ProductionMass: ResolveFragmentAccessors - resolved StaticStruct=0x%llX or "
					"GetFragmentDataPtr=0x%llX fall outside the main module; offsets are likely wrong - "
					"aborting before they're called",
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
		uintptr_t ResolveSignalEntity()
		{
			IPluginScanner* scanner = GetScanner();
			if (!scanner)
				return 0;

			uintptr_t addr = scanner->FindPatternInMainModule(Signatures::MassSignalSubsystem_SignalEntity);
			if (!addr)
				LOG_WARN("ProductionMass: UMassSignalSubsystem::SignalEntity pattern not found");

			return addr;
		}
	}

	bool Init(IPluginSelf* self, CraftingCompleteCallback callback)
	{
		IPluginScanner* scanner = GetScanner();
		if (!scanner || !self->hooks->Hooks)
		{
			LOG_WARN("ProductionMass: scanner/hooks unavailable - Mass-simulated crafting will not be tracked");
			return false;
		}

		if (!ResolveGetMassEntitySubsystem())
			return false;

		if (!ResolveFragmentAccessors())
			return false;

		uintptr_t signalEntityAddr = ResolveSignalEntity();
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

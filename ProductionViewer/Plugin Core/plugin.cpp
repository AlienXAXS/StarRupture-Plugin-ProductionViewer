#include "plugin.h"
#include "Helpers/plugin_helpers.h"
#include "Config/plugin_config.h"
#include "Storage/production_storage.h"
#include "Production Viewer/production_ui.h"
#include "Production Viewer/production_tracker.h"
#include "Production Viewer/production_icons.h"
#include "Engine_classes.hpp"

// Global plugin self pointer — stable for the plugin's lifetime, retained from PluginInit
static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

// Plugin metadata
#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

static PluginInfo s_pluginInfo = {
	"ProductionViewer",
	MODLOADER_BUILD_TAG,
	"AlienX",
	"Displays production information",
	PLUGIN_INTERFACE_VERSION,
	PLUGIN_TARGET_CLIENT
};

// Fires once a save is fully loaded into the world — (re)load this session's
// production data so "All Time" totals continue from where they left off.
static void OnExperienceLoadComplete()
{
	SDK::UWorld* world = SDK::UWorld::GetWorld();
	if (!world || world->GetName() != "ChimeraMain")
	{
		LOG_DEBUG("OnExperienceLoadComplete: ignored (not in ChimeraMain map)");
		return;
	}

	if (ProductionViewer::Storage::Reload())
		ProductionTracker::OnSessionLoaded();
}

extern "C" {

	__declspec(dllexport) PluginInfo* GetPluginInfo()
	{
		return &s_pluginInfo;
	}

	__declspec(dllexport) bool PluginInit(IPluginSelf* self)
	{
		// Store the plugin self pointer — valid for the plugin's entire lifetime
		g_self = self;

		LOG_INFO("Plugin initializing...");

		// Initialize config system
		ProductionViewerConfig::Config::Initialize(self);

		// Check if plugin is enabled via config
		if (!ProductionViewerConfig::Config::IsEnabled())
		{
			LOG_WARN("Plugin is disabled in config file");
			return true; // Return true so plugin loads but doesn't activate
		}

		// Resolve the per-plugin data folder (<Plugins>\ProductionViewer\)
		ProductionViewer::Storage::Initialize(self);

		// Register the Production Viewer ImGui window
		ProductionUI::Init(self);

		// Start tracking production/consumption via the crafting hook
		ProductionTracker::Init(self);

		// Kick off the asset-registry scan that pre-loads item/recipe icons
		ProductionIcons::Init(self);

		if (self->hooks->World)
			self->hooks->World->RegisterOnExperienceLoadComplete(&OnExperienceLoadComplete);

		// Hot-reload: experience-load-complete may have already fired before we
		// registered, so if a session is already in progress, load it now.
		OnExperienceLoadComplete();

		LOG_INFO("Plugin initialized successfully");

		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("Plugin shutting down...");

		if (g_self && g_self->hooks->World)
			g_self->hooks->World->UnregisterOnExperienceLoadComplete(&OnExperienceLoadComplete);

		ProductionTracker::Shutdown(g_self);
		ProductionIcons::Shutdown();
		ProductionUI::Shutdown(g_self);
		ProductionViewer::Storage::Shutdown();

		g_self = nullptr;
	}

} // extern "C"

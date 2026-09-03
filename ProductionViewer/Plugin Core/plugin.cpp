#include "plugin.h"
#include "Helpers/plugin_helpers.h"
#include "Config/plugin_config.h"
#include "Net/production_net.h"
#include "Storage/production_storage.h"
#include "Production Viewer/production_ui.h"
#include "Production Viewer/production_tracker.h"
#include "Production Viewer/production_icons.h"
#include "Production Viewer/production_waypoint.h"
#include "Engine_classes.hpp"

// Global plugin self pointer — stable for the plugin's lifetime, retained from PluginInit
static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

// Plugin metadata
#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

// The loader refuses a DLL whose target doesn't match its own build. The server
// build carries no UI at all — it exists to track the whole base and feed the
// clients, which are the only ones that draw anything.
#if defined(MODLOADER_SERVER_BUILD)
#define PLUGIN_TARGET_THIS PLUGIN_TARGET_SERVER
static const char* kBuildFlavour = "server";
#else
#define PLUGIN_TARGET_THIS PLUGIN_TARGET_CLIENT
static const char* kBuildFlavour = "client";
#endif

// The name doubles as the network routing key — the client and server builds
// must agree on it exactly or every packet is silently dropped.
static PluginInfo s_pluginInfo = {
	"ProductionViewer",
	MODLOADER_BUILD_TAG,
	"AlienX",
	"Displays production information",
	PLUGIN_INTERFACE_VERSION,
	PLUGIN_TARGET_THIS
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

	// Called unconditionally, even when there is no session file to reload: a
	// client has none (its data comes off the wire) but still has to drop the
	// previous world's numbers, and the server has to tell its clients to.
	if (!ProductionViewer::Storage::Reload())
		LOG_DEBUG("OnExperienceLoadComplete: no session storage resolved - starting from empty");

	ProductionTracker::OnSessionLoaded();
}

extern "C" {

	__declspec(dllexport) PluginInfo* GetPluginInfo()
	{
		return &s_pluginInfo;
	}

	// Runs after GetPluginInfo and before PluginInit, and is the only context in
	// which the loader lets a plugin pattern scan. Resolve here, install from
	// PluginInit — self->hooks is null for the duration of this event.
	__declspec(dllexport) void OnPluginLoadHooks(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		g_self = self;
		ProductionTracker::ResolvePatterns(self, scanner);
	}

	__declspec(dllexport) bool PluginInit(IPluginSelf* self)
	{
		// Store the plugin self pointer — valid for the plugin's entire lifetime
		g_self = self;

		LOG_INFO("Plugin initializing (%s build)...", kBuildFlavour);

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

		// Register the packet handlers before anything can produce or receive
		// data. The server is the sole authority in multiplayer; clients track
		// nothing of their own and display only what arrives from here.
		ProductionNet::Init(self);

		// Register the Production Viewer ImGui window
		ProductionUI::Init(self);

		// Register the "walk to this base" overlay, triggered from a base
		// breakdown row's "Go" button
		ProductionWaypoint::Init(self);

		// Start tracking production/consumption via the crafting hook
		ProductionTracker::Init(self);

		// Kick off the asset-registry scan that pre-loads item/recipe icons
		ProductionIcons::Init(self);

		if (self->hooks->World)
			self->hooks->World->RegisterOnExperienceLoadComplete(&OnExperienceLoadComplete);

		// Note: the join-time snapshot is NOT driven from a player-joined hook.
		// ProductionNet::Init registers for client-ready instead, which is the
		// first moment a packet to that client can actually be delivered.

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

		ProductionNet::Shutdown(g_self);
		ProductionTracker::Shutdown(g_self);
		ProductionIcons::Shutdown();
		ProductionWaypoint::Shutdown(g_self);
		ProductionUI::Shutdown(g_self);
		ProductionViewer::Storage::Shutdown();

		g_self = nullptr;
	}

} // extern "C"

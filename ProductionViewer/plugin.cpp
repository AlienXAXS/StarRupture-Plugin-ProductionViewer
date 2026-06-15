#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"

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
	"Your Name",
	"Displays production information",
	PLUGIN_INTERFACE_VERSION
};

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

		// Your initialization code would go here
		// Examples:
		// - Find memory patterns with scanner
		// - Install hooks with hooks interface
		// - Register callbacks

		LOG_INFO("Plugin initialized successfully");

		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("Plugin shutting down...");

		// Your cleanup code would go here
		// Examples:
		// - Remove installed hooks
		// - Free allocated resources
		// - Unregister callbacks

		g_self = nullptr;
	}

} // extern "C"

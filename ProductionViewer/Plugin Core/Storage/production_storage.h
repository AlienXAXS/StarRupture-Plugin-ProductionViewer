#pragma once

#include "plugin_interface.h"
#include "../Vendor/json.hpp"

#include <string>

// Session-aware JSON storage for the Production Viewer plugin.
//
// Mirrors the pattern used by the BetterCheats plugin's SessionConfig: data is
// written to <exe_dir>\Plugins\<pluginName>\<SessionName>.json, so production
// history follows the active save and doesn't bleed between different worlds.
namespace ProductionViewer::Storage
{
	// Resolves and creates the plugin's data folder (<Plugins>\<pluginName>\) —
	// call once during PluginInit.
	void Initialize(IPluginSelf* self);
	void Shutdown();

	// Resolves the active save's session name and loads its JSON data from
	// <Plugins>\<pluginName>\<SessionName>.json. Must run on the game thread
	// (e.g. from OnExperienceLoadComplete). Returns false if no session is
	// currently active.
	bool Reload();

	// True once Reload() has resolved a session and loaded (or created) its data.
	bool IsLoaded();

	std::string GetSessionName();

	// Reads the value at a dot-separated path (e.g. "items.IronPlate.production").
	// Returns defaultValue if no session is loaded or the path doesn't exist.
	nlohmann::json Get(const std::string& path, const nlohmann::json& defaultValue);

	// Writes the value at a dot-separated path in memory only. Call Save() to
	// persist to disk. No-op if no session is currently loaded.
	void Set(const std::string& path, const nlohmann::json& value);

	// Persists the current in-memory data to <SessionName>.json. No-op if no
	// session is currently loaded.
	void Save();
}

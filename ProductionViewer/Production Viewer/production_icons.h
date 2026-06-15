#pragma once

#include "plugin_interface.h"

#include <string>

// Pre-loads item/recipe icons (UTexture2D -> ImGui texture handle) for every
// item type in the game, keyed by UAuItemDataBase::UniqueItemName.
//
// Mirrors BetterCheats' Panels::Items icon-loading pattern: the asset
// registry scan and UTexture2D -> GPU texture copy happen on the game thread
// (posted from Init), with retries driven from OnEngineTick in case D3D12
// wasn't ready yet. The splash screen is held open (AcquireSplashHold) until
// the initial scan + texture copy completes, so the user sees load progress.
namespace ProductionIcons
{
	void Init(IPluginSelf* self);
	void Shutdown();

	// Adopts any freshly-scanned icon list and retries texture loads that
	// returned NULL earlier (D3D12 not ready yet). Call once per engine tick.
	void Tick();

	// Returns a texture handle for the given UniqueItemName, or nullptr if
	// the item has no icon, hasn't been resolved yet, or its texture hasn't
	// loaded yet.
	PluginTextureHandle GetIcon(const std::string& uniqueItemName);
}

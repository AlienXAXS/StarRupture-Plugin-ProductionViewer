#pragma once

#include "plugin_interface.h"

#include <string>

// Item icons for the tracker's rows (UTexture2D -> ImGui texture handle), keyed
// by UAuItemDataBase::UniqueItemName. The SDK's item_icons.h does the loading:
// an icon is copied the first time a row draws it, a few per engine tick.
namespace ProductionIcons
{
	void Init(IPluginSelf* self);
	void Shutdown();

	// Loads the icons rows have asked for. Call once per engine tick.
	void Tick();

	// Draws the item's icon at the cursor, then moves on by gap. An icon that
	// is still loading leaves an empty square, and an item with no icon takes
	// no room. Call from a render callback.
	void Draw(IModLoaderImGui* imgui, IPluginImGuiTextures* textures,
		const std::string& uniqueItemName, float size, float gap);
}

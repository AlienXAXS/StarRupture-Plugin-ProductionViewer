#pragma once

#include "plugin_interface.h"

#include <string>

// Item icons for the tracker's rows. The loader's image library does the
// loading; this only registers the item set and names an item's image.
namespace ProductionIcons
{
	// Tells the library the item set is drawn from. Call once at start.
	void Init(IPluginSelf* self);

	// The library's id for an item's image, or 0 when the loader has no image
	// library, or the name is empty or stands for an item that could not be read.
	// Any thread; reads nothing.
	PluginImageId IdOf(const std::string& uniqueItemName);
}

#pragma once

#include "plugin_interface.h"

#include <cstdint>
#include <string>

// Drives a small moveable overlay window that points the player toward a
// base core they picked from the item breakdown (ProductionUI's "Go"
// button). Rendered via IPluginUIEvents::RegisterWidget so it stays visible
// even while the main Production Viewer panel is closed.
namespace ProductionWaypoint
{
	// Registers the overlay widget. Call once from PluginInit, after
	// ProductionBaseCore::Init (the widget reads its cached base locations).
	void Init(IPluginSelf* self);

	// Unregisters the widget. Call from PluginShutdown.
	void Shutdown(IPluginSelf* self);

	// Starts navigating to the given base core (key from
	// ProductionBaseCore::PackHandle) and shows the overlay. Replaces any
	// previously active target.
	void SetTarget(uint64_t baseKey, const std::string& baseName);

	// Stops navigating and hides the overlay, if active.
	void ClearTarget();
}

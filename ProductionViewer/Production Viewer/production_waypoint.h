#pragma once

#include "plugin_interface.h"

#include <cstdint>
#include <string>

// Drives a small moveable overlay window that points the player toward a
// base core they picked from the item breakdown (ProductionUI's "Go"
// button). Rendered via IPluginUIEvents::RegisterWidget so it stays visible
// even while the main Production Viewer panel is closed. The arrow, distance
// and pose sampling come from the SDK's waypoint helper; this module only
// finds the base core again each tick.
namespace ProductionWaypoint
{
	// Registers the overlay widget. Call once from PluginInit, after
	// ProductionBaseCore::Init (the widget reads its cached base locations).
	void Init(IPluginSelf* self);

	// Unregisters the widget. Call from PluginShutdown.
	void Shutdown(IPluginSelf* self);

	// Resolves the target's current position and name and samples the camera
	// and the local player for the overlay. Call every tick from the game
	// thread: the render callback cannot read UObjects.
	void Tick();

	// A world was loaded: the target's key no longer means anything. The overlay
	// keeps its last position and looks for the base within 100 cm of it,
	// showing "Location lost." and closing after about a minute if none shows up.
	// Call after ProductionBaseCore::OnSessionLoaded.
	void OnSessionLoaded();

	// False while the waypoint is off: the target is kept, nothing is drawn.
	bool IsOn();

	// The world is ending. A target kept while the waypoint is off is dropped.
	void OnSessionEnded();

	// Drops the target, if any, and hides the overlay. Game thread only.
	void Clear();

	// Starts navigating to the given base core (key from
	// ProductionBaseCore::PackHandle) and shows the overlay. Replaces any
	// previously active target.
	void SetTarget(uint64_t baseKey, const std::string& baseName);
}

#pragma once

#include "plugin_interface.h"

// Registers and drives the Production Viewer ImGui window.
namespace ProductionUI
{
	// Registers the panel (and toggle keybind, if hooks->Input is available).
	// Call once from PluginInit, after the config system has been initialized.
	void Init(IPluginSelf* self);

	// Game thread, once per tick. Closes the window when the viewer turns off,
	// and does the Go button's calls to the Go to Item plugin.
	void Tick();

	// Game thread. Forgets the Go button's waiting click and what Tick last
	// published, for a world change or a shutdown.
	void ResetGo();

	// Unregisters the panel and toggle keybind. Call from PluginShutdown.
	void Shutdown(IPluginSelf* self);
}

#pragma once

#include "plugin_interface.h"

// Registers and drives the Production Viewer ImGui window.
namespace ProductionUI
{
	// Registers the panel (and toggle keybind, if hooks->Input is available).
	// Call once from PluginInit, after the config system has been initialized.
	void Init(IPluginSelf* self);

	// Unregisters the panel and toggle keybind. Call from PluginShutdown.
	void Shutdown(IPluginSelf* self);
}

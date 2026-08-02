#pragma once

#include "plugin_interface.h"

namespace ProductionViewerConfig
{
	static const ConfigEntry CONFIG_ENTRIES[] = {
		{
			"General",
			"Enabled",
			ConfigValueType::Boolean,
			"true",
			"Enable or disable ProductionViewer"
		},
		{
			"Menu",
			"ToggleKey",
			ConfigValueType::Keybind,
			"P",
			"Key to open / close the ProductionViewer menu"
		},
		{
			"Network",
			"BroadcastInterval",
			ConfigValueType::Float,
			"1.0",
			"Seconds between production updates sent from the server to clients"
		},
		{
			"Network",
			"StaleAfterSeconds",
			ConfigValueType::Float,
			"10.0",
			"Seconds without a server update before a client flags its production data as stale"
		}
	};

	static const ConfigSchema SCHEMA = {
		CONFIG_ENTRIES,
		sizeof(CONFIG_ENTRIES) / sizeof(ConfigEntry)
	};

	// Type-safe config accessor class
	class Config
	{
	public:
		static void Initialize(IPluginSelf* self)
		{
			s_self = self;

			// Initialize config from schema - creates file with defaults if missing
			if (s_self)
			{
				s_self->config->InitializeFromSchema(s_self, &SCHEMA);
			}
		}

		static bool IsEnabled()
		{
			return s_self ? s_self->config->ReadBool(s_self, "General", "Enabled", true) : true;
		}

		// Returns the current toggle keybind string (e.g. "P", "Ctrl+F10").
		// The modloader re-registers the keybind automatically when the user changes it.
		static const char* GetToggleKey()
		{
			static char buffer[64];
			if (s_self && s_self->config->ReadString(s_self, "Menu", "ToggleKey", buffer, sizeof(buffer), "P"))
				return buffer;
			return "P";
		}

		// How often the server pushes production updates to clients. Clamped:
		// faster than a few times a second buys nothing (the panel itself only
		// refreshes once a second) and just burns bandwidth.
		static float GetBroadcastInterval()
		{
			const float value = s_self ? s_self->config->ReadFloat(s_self, "Network", "BroadcastInterval", 1.0f) : 1.0f;
			return value < 0.25f ? 0.25f : (value > 30.0f ? 30.0f : value);
		}

		// How long a client tolerates silence before saying so. Must stay above
		// the server's heartbeat cadence or an idle base reads as a dead link.
		static float GetStaleAfterSeconds()
		{
			const float value = s_self ? s_self->config->ReadFloat(s_self, "Network", "StaleAfterSeconds", 10.0f) : 10.0f;
			return value < 6.0f ? 6.0f : (value > 300.0f ? 300.0f : value);
		}

	private:
		static IPluginSelf* s_self;
	};
}

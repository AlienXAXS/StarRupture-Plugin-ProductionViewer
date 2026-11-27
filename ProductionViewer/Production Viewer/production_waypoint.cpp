#include "production_waypoint.h"
#include "production_basecore.h"
#include "production_tracker.h"
#include "production_fields.h"
#include "Plugin Core/Helpers/plugin_helpers.h"

#include "Engine_classes.hpp"
#include "plugin_map.h"
#include "plugin_waypoint.h"

#include <chrono>
#include <limits>
#include <mutex>

namespace ProductionWaypoint
{
	namespace
	{
		// After a world change the old key means nothing. The target is looked
		// for again by position: base cores don't move, so the one within this
		// distance of where it stood is the same base. If none shows up in this
		// long, the overlay gives up.
		constexpr double kRebindRadiusCm = 100.0;
		constexpr double kSearchTimeoutSeconds = 60.0;

		IPluginSelf* g_self = nullptr;
		WidgetHandle g_widget = nullptr;

		// The target, guarded by g_mutex. Only the resolver (game thread, from
		// the waypoint's Tick) and SetTarget / OnSessionLoaded touch it; the
		// render callback never does. g_mutex is always taken before the
		// waypoint's own lock, never the other way round.
		std::mutex g_mutex;
		uint32_t g_targetSerial = 0;   // bumped by SetTarget; a stale resolver result is dropped
		uint64_t g_targetKey = 0;
		std::string g_targetName;      // click-time name until the first live one arrives
		bool g_searching = false;      // world changed; looking for the base by position
		bool g_locValid = false;
		SDK::FVector g_loc{};          // last known position of the base
		std::chrono::steady_clock::time_point g_searchStart;
		bool g_paused = false;         // the waypoint is off; the target waits until it is back on

		PluginWaypoint::Options MakeOptions()
		{
			PluginWaypoint::Options options;
			// The resolver closes the overlay itself (a demolished base at once,
			// a base that never reappears after a world change after
			// kSearchTimeoutSeconds), so the helper's tick-count grace is off.
			options.lostGraceTicks = (std::numeric_limits<int>::max)();
			return options;
		}

		PluginWaypoint::Waypoint g_waypoint(MakeOptions());

		// A paused target belongs to the world it was set in: nothing resolves it
		// while paused, so it goes when that world does. The features are read
		// too, for a world that ends before a tick has paused the target.
		bool ClearIfPausedLocked()
		{
			if (!g_paused && IsOn())
				return false;
			g_paused = false;
			g_waypoint.Clear();
			return true;
		}

		// Closes the overlay if the target is still the one the caller resolved.
		// The widget is hidden by the render callback, which sees Render() fail.
		void CloseIfCurrent(uint32_t serial, const char* why)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_targetSerial != serial)
				return;
			LOG_DEBUG("ProductionWaypoint: closing - %s (Key=%llu)", why,
				static_cast<unsigned long long>(g_targetKey));
			g_waypoint.Clear();
		}

		// Game thread, once per tick while active. Finds the target's current
		// position (rebinding by position after a world change) and its current
		// name, as plain data.
		bool ResolveTarget(void* /*user*/, SDK::UWorld* /*world*/, PluginWaypoint::Resolved& out)
		{
			uint32_t serial;
			uint64_t key;
			bool searching;
			bool locValid;
			SDK::FVector loc;
			std::string label;
			std::chrono::steady_clock::time_point searchStart;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				serial = g_targetSerial;
				key = g_targetKey;
				searching = g_searching;
				locValid = g_locValid;
				loc = g_loc;
				label = g_targetName;
				searchStart = g_searchStart;
			}

			const bool readSearching = searching;
			bool found = false;
			if (searching)
			{
				uint64_t foundKey = 0;
				SDK::FVector foundLoc{};
				if (locValid && ProductionBaseCore::FindBaseNear(loc, kRebindRadiusCm, foundKey)
					&& ProductionBaseCore::GetBaseCoreLocation(foundKey, foundLoc))
				{
					LOG_DEBUG("ProductionWaypoint: rebound Key=%llu -> Key=%llu after the world change",
						static_cast<unsigned long long>(key), static_cast<unsigned long long>(foundKey));
					key = foundKey;
					loc = foundLoc;
					searching = false;
					found = true;
				}
				else
				{
					const std::chrono::duration<double> waited = std::chrono::steady_clock::now() - searchStart;
					if (waited.count() >= kSearchTimeoutSeconds)
					{
						CloseIfCurrent(serial, "no base near the old location after the world change");
						return false;
					}
				}
			}
			else
			{
				SDK::FVector current{};
				if (!ProductionBaseCore::GetBaseCoreLocation(key, current))
				{
					CloseIfCurrent(serial, "the base core is gone");
					return false;
				}
				loc = current;
				locValid = true;
				found = true;
			}

			if (found)
			{
				// The name follows renames: this process's own read when it
				// resolves base cores itself, the server's announcement on a client.
				std::string name;
				if (ProductionBaseCore::GetBaseName(key, name) || ProductionTracker::GetRemoteBaseName(key, name))
					label = std::move(name);
			}

			{
				// SetTarget may have replaced the target while this tick resolved
				// the old one; a world change landing in the same window (search
				// state no longer what was read) takes precedence too.
				std::lock_guard<std::mutex> lock(g_mutex);
				if (g_targetSerial == serial && g_searching == readSearching)
				{
					g_targetKey = key;
					g_searching = searching;
					g_locValid = locValid;
					g_loc = loc;
					g_targetName = label;
				}
			}

			if (!found)
				return false;   // still searching: the overlay says "Location lost."

			out.x = loc.X;
			out.y = loc.Y;
			out.z = loc.Z;
			out.label = std::move(label);
			return true;
		}

		void HideWidget()
		{
			if (g_self && g_self->hooks->UI && g_widget)
				g_self->hooks->UI->SetWidgetVisible(g_widget, false);
		}

		void RenderWaypointWidget(IModLoaderImGui* imgui)
		{
			// Tick hides the widget and shows it again. Hiding it here could
			// leave it hidden if the waypoint came back on before that tick.
			if (!IsOn())
				return;

			imgui->SetWindowFontScale(1.15f);
			if (!g_waypoint.Render(imgui))
				HideWidget();
		}
	}

	// The waypoint points at base cores, so it is off when either part is.
	bool IsOn()
	{
		return ProductionViewerFields::BaseWaypoint.On() && ProductionViewerFields::BaseCores.On();
	}

	void Init(IPluginSelf* self)
	{
		g_self = self;

		if (!self->hooks->UI)
		{
			LOG_WARN("ProductionWaypoint: UI hooks unavailable (server/generic build) - waypoint overlay not registered");
			return;
		}

		// Keeping the title bar (default chrome) gives the window a drag
		// handle so it stays moveable, per the feature request - there's no
		// p_open plumbed through for widgets, so the visible "X##..." button
		// in the render callback is the only close affordance.
		static PluginWindowHints hints = { 280.0f, 0.0f, -1.0f, -1.0f, 0.0f, 0.0f, 1 /*FirstUseEver*/, 1 /*FirstUseEver*/,
			PluginWindowFlags_NoScrollbar };
		// "Display Label###StableID" - ImGui splits on "###" so the title bar
		// shows a friendly name while the window keeps a stable identity.
		static PluginWidgetDesc desc = { "Base Waypoint###ProductionViewer_Waypoint", &RenderWaypointWidget, &hints };
		g_widget = self->hooks->UI->RegisterWidget(&desc);

		// Widgets default to visible - stay hidden until SetTarget() is
		// called from a base breakdown row's "Go" button.
		if (g_widget)
			self->hooks->UI->SetWidgetVisible(g_widget, false);

		LOG_DEBUG("ProductionWaypoint: RegisterWidget returned handle=%p", g_widget);
	}

	void Shutdown(IPluginSelf* self)
	{
		if (self && self->hooks->UI && g_widget)
		{
			self->hooks->UI->UnregisterWidget(g_widget);
			g_widget = nullptr;
		}

		g_waypoint.Clear();
		g_paused = false;
		g_self = nullptr;
	}

	void OnSessionLoaded()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (ClearIfPausedLocked() || !g_waypoint.Active())
			return;

		// Handles from the old world are meaningless; keep the last position and
		// look for the base there once the new world's locations arrive.
		if (g_locValid)
		{
			g_searching = true;
			g_searchStart = std::chrono::steady_clock::now();
		}
		else
			g_waypoint.Clear();
	}

	void OnSessionEnded()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		ClearIfPausedLocked();
	}

	void Clear()
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_paused = false;
			g_waypoint.Clear();
		}
		HideWidget();
	}

	void Tick()
	{
		const bool on = IsOn();
		bool hide = false;
		bool show = false;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!on && !g_paused && g_waypoint.Active())
			{
				g_paused = true;
				hide = true;
			}
			else if (on && g_paused)
			{
				g_paused = false;
				// Time spent off doesn't count against the search for the base.
				if (g_searching)
					g_searchStart = std::chrono::steady_clock::now();
				show = g_waypoint.Active();
			}
		}

		if (hide)
			HideWidget();
		if (show && g_self && g_self->hooks->UI && g_widget)
			g_self->hooks->UI->SetWidgetVisible(g_widget, true);

		if (on)
			g_waypoint.Tick(PluginSdkRuntime::GetWorld());
	}

	void SetTarget(uint64_t baseKey, const std::string& baseName)
	{
		if (!IsOn())
			return;

		// The row that was clicked has a cached location, so a world change
		// right after the click still has a position to search from.
		SDK::FVector loc{};
		const bool locValid = ProductionBaseCore::GetBaseCoreLocation(baseKey, loc);

		{
			std::lock_guard<std::mutex> lock(g_mutex);
			++g_targetSerial;
			g_targetKey = baseKey;
			g_targetName = baseName;
			g_searching = false;
			g_locValid = locValid;
			g_loc = loc;
			g_waypoint.Set(&ResolveTarget, nullptr, baseName);
		}

		LOG_DEBUG("ProductionWaypoint: SetTarget - now navigating to '%s' (Key=%llu)",
			baseName.c_str(), static_cast<unsigned long long>(baseKey));

		if (g_self && g_self->hooks->UI && g_widget)
			g_self->hooks->UI->SetWidgetVisible(g_widget, true);
	}
}

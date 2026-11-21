#include "production_waypoint.h"
#include "production_basecore.h"
#include "production_tracker.h"
#include "Plugin Core/Helpers/plugin_helpers.h"

#include "Engine_classes.hpp"
#include "plugin_map.h"

#include <chrono>
#include <cmath>
#include <mutex>

namespace ProductionWaypoint
{
	namespace
	{
		// Size (px) of the square area reserved for the compass arrow, and
		// how far the tip and back corners sit from its center.
		constexpr float kArrowAreaSize = 90.0f;
		constexpr float kArrowTipRadius = 38.0f;
		constexpr float kArrowBackRadius = 20.0f;
		constexpr float kArrowHalfWidth = 20.0f;

		// Once the player is within this many meters of the target, the
		// overlay closes itself - no point pointing at something you're
		// already standing on.
		constexpr float kAutoCloseDistanceMeters = 10.0f;

		// After a world change the old key means nothing. The target is looked
		// for again by position: base cores don't move, so the one within this
		// distance of where it stood is the same base. If none shows up in this
		// long, the overlay gives up.
		constexpr double kRebindRadiusCm = 100.0;
		constexpr double kSearchTimeoutSeconds = 60.0;

		enum class TargetState : uint8_t
		{
			Tracking,    // the key resolves; the arrow points at it
			Searching,   // world changed; looking for the base by position
			Gone,        // base demolished or never found again; the render thread closes the overlay
		};

		IPluginSelf* g_self = nullptr;
		WidgetHandle g_widget = nullptr;

		// Everything below is guarded by g_mutex. Tick() resolves the target on
		// the game thread and publishes the result here, so the render callback
		// only reads plain data.
		std::mutex g_mutex;
		bool g_active = false;
		uint32_t g_targetSerial = 0;   // bumped by SetTarget; a stale Tick result is dropped
		uint64_t g_targetKey = 0;
		std::string g_targetName;      // click-time name until the first live one arrives
		TargetState g_state = TargetState::Tracking;
		bool g_targetLocValid = false;
		SDK::FVector g_targetLoc{};    // last known position of the base
		std::chrono::steady_clock::time_point g_searchStart;

		// The local player's pose, sampled by Tick() on the game thread. The
		// render callback runs on the render thread, where UObjects must not
		// be touched, so it only reads this copy.
		bool g_poseValid = false;
		SDK::FVector g_playerLoc{};
		double g_playerYaw = 0.0;

		// Draws an isoceles arrow centered in a kArrowAreaSize square reserved
		// at the current layout cursor, pointing at relativeDegrees (0 =
		// straight up/forward, positive = clockwise/right - matches the
		// player-relative bearing computed in RenderWaypointWidget). Uses the
		// v48 ImDrawList access so the arrow rotates smoothly instead of
		// snapping to the 4 directions ArrowButton is limited to.
		void DrawCompassArrow(IModLoaderImGui* imgui, double relativeDegrees)
		{
			float originX, originY;
			imgui->GetCursorScreenPos(&originX, &originY);
			imgui->Dummy(kArrowAreaSize, kArrowAreaSize);

			const float centerX = originX + kArrowAreaSize * 0.5f;
			const float centerY = originY + kArrowAreaSize * 0.5f;

			// Screen space: up is -Y, right is +X. At angle 0 this points up;
			// increasing angle (clockwise) sweeps toward +X, matching the
			// bearing convention used by the caller.
			const double angleRad = relativeDegrees * 3.14159265358979323846 / 180.0;
			const float dirX = static_cast<float>(std::sin(angleRad));
			const float dirY = static_cast<float>(-std::cos(angleRad));
			// Perpendicular to (dirX, dirY).
			const float perpX = -dirY;
			const float perpY = dirX;

			const float tipX = centerX + dirX * kArrowTipRadius;
			const float tipY = centerY + dirY * kArrowTipRadius;
			const float backCenterX = centerX - dirX * kArrowBackRadius;
			const float backCenterY = centerY - dirY * kArrowBackRadius;
			const float leftX = backCenterX + perpX * kArrowHalfWidth;
			const float leftY = backCenterY + perpY * kArrowHalfWidth;
			const float rightX = backCenterX - perpX * kArrowHalfWidth;
			const float rightY = backCenterY - perpY * kArrowHalfWidth;

			PluginDrawList dl = imgui->GetWindowDrawList();
			const unsigned int fillColor = imgui->GetColorU32FromVec4(0.95f, 0.65f, 0.15f, 1.0f);
			const unsigned int outlineColor = imgui->GetColorU32FromVec4(0.0f, 0.0f, 0.0f, 0.6f);
			imgui->DL_AddTriangleFilled(dl, tipX, tipY, leftX, leftY, rightX, rightY, fillColor);
			imgui->DL_AddTriangle(dl, tipX, tipY, leftX, leftY, rightX, rightY, outlineColor, 2.0f);
		}

		void RenderWaypointWidget(IModLoaderImGui* imgui)
		{
			uint64_t targetKey;
			std::string targetName;
			TargetState state;
			bool targetLocValid;
			SDK::FVector targetLoc;
			bool poseValid;
			SDK::FVector playerLoc;
			double playerYaw;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				if (!g_active)
					return;
				targetKey = g_targetKey;
				targetName = g_targetName;
				state = g_state;
				targetLocValid = g_targetLocValid;
				targetLoc = g_targetLoc;
				poseValid = g_poseValid;
				playerLoc = g_playerLoc;
				playerYaw = g_playerYaw;
			}

			if (state == TargetState::Gone)
			{
				LOG_DEBUG("ProductionWaypoint: render - base core Key=%llu is gone - closing",
					static_cast<unsigned long long>(targetKey));
				ClearTarget();
				return;
			}

			imgui->SetWindowFontScale(1.15f);

			imgui->Text(targetName.c_str());
			imgui->SameLine(0.0f, 12.0f);
			if (imgui->SmallButton("X##ProductionViewer_WaypointClose"))
			{
				LOG_DEBUG("ProductionWaypoint: closed via X button (targetKey=%llu)",
					static_cast<unsigned long long>(targetKey));
				ClearTarget();
				return;
			}

			imgui->Separator();

			if (state == TargetState::Searching)
			{
				imgui->TextDisabled("Location lost.");
				return;
			}

			if (!poseValid)
			{
				imgui->TextDisabled("Player unavailable.");
				LOG_TRACE("ProductionWaypoint: render - no player pose yet");
				return;
			}

			if (!targetLocValid)
				return;

			const double dx = targetLoc.X - playerLoc.X;
			const double dy = targetLoc.Y - playerLoc.Y;
			const double dz = targetLoc.Z - playerLoc.Z;

			// World units are centimeters.
			const double distanceMeters = std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0;

			if (distanceMeters <= kAutoCloseDistanceMeters)
			{
				LOG_DEBUG("ProductionWaypoint: render - reached target (Key=%llu, %.1fm) - auto-closing",
					static_cast<unsigned long long>(targetKey), distanceMeters);
				ClearTarget();
				return;
			}

			// FVector::Rotation()'s Yaw convention: atan2(Y, X), degrees.
			const double targetYaw = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
			double relative = targetYaw - playerYaw;
			while (relative > 180.0)
				relative -= 360.0;
			while (relative < -180.0)
				relative += 360.0;

			LOG_TRACE("ProductionWaypoint: render - Key=%llu dist=%.1fm playerYaw=%.1f targetYaw=%.1f relative=%.1f",
				static_cast<unsigned long long>(targetKey), distanceMeters, playerYaw, targetYaw, relative);

			DrawCompassArrow(imgui, relative);

			char label[32];
			snprintf(label, sizeof(label), "%.0fm", distanceMeters);
			imgui->Text(label);
		}
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

		std::lock_guard<std::mutex> lock(g_mutex);
		g_active = false;
		g_self = nullptr;
	}

	// Resolves the target for this tick: its position (rebinding by position
	// after a world change) and its current name. Game thread; the result is
	// published for the render callback by the caller.
	static void ResolveTarget(uint64_t& key, TargetState& state, bool& locValid, SDK::FVector& loc,
		std::chrono::steady_clock::time_point searchStart, std::string& label)
	{
		if (state == TargetState::Searching)
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
				state = TargetState::Tracking;
			}
			else
			{
				const std::chrono::duration<double> waited = std::chrono::steady_clock::now() - searchStart;
				if (waited.count() >= kSearchTimeoutSeconds)
				{
					LOG_DEBUG("ProductionWaypoint: no base within %.0f cm of the old location after %.0f s - giving up",
						kRebindRadiusCm, kSearchTimeoutSeconds);
					state = TargetState::Gone;
				}
				return;
			}
		}
		else
		{
			SDK::FVector current{};
			if (ProductionBaseCore::GetBaseCoreLocation(key, current))
			{
				loc = current;
				locValid = true;
			}
			else
			{
				state = TargetState::Gone;
				return;
			}
		}

		// The name follows renames: this process's own read when it resolves
		// base cores itself, the server's announcement on a client.
		std::string name;
		if (ProductionBaseCore::GetBaseName(key, name) || ProductionTracker::GetRemoteBaseName(key, name))
			label = std::move(name);
	}

	void OnSessionLoaded()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_active)
			return;

		// Handles from the old world are meaningless; keep the last position and
		// look for the base there once the new world's locations arrive.
		if (g_targetLocValid)
		{
			g_state = TargetState::Searching;
			g_searchStart = std::chrono::steady_clock::now();
		}
		else
			g_state = TargetState::Gone;
	}

	void Tick()
	{
		uint32_t serial;
		uint64_t key;
		TargetState state;
		bool locValid;
		SDK::FVector targetLoc;
		std::string label;
		std::chrono::steady_clock::time_point searchStart;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_active)
				return;
			serial = g_targetSerial;
			key = g_targetKey;
			state = g_state;
			locValid = g_targetLocValid;
			targetLoc = g_targetLoc;
			label = g_targetName;
			searchStart = g_searchStart;
		}

		const TargetState readState = state;
		if (state != TargetState::Gone)
			ResolveTarget(key, state, locValid, targetLoc, searchStart, label);

		SDK::UWorld* world = PluginSdkRuntime::GetWorld();
		SDK::APawn* pawn = world ? SDK::UGameplayStatics::GetPlayerPawn(world, 0) : nullptr;

		bool valid = false;
		SDK::FVector loc{};
		double yaw = 0.0;
		if (pawn)
		{
			loc = pawn->K2_GetActorLocation();
			yaw = pawn->GetControlRotation().Yaw;
			valid = true;
		}

		std::lock_guard<std::mutex> lock(g_mutex);
		g_poseValid = valid;
		g_playerLoc = loc;
		g_playerYaw = yaw;

		// SetTarget may have replaced the target while this tick resolved the
		// old one.
		// A world change landing in the same window (state no longer what was
		// read) takes precedence too.
		if (g_active && g_targetSerial == serial && g_state == readState)
		{
			g_targetKey = key;
			g_state = state;
			g_targetLocValid = locValid;
			g_targetLoc = targetLoc;
			g_targetName = std::move(label);
		}
	}

	void SetTarget(uint64_t baseKey, const std::string& baseName)
	{
		// The row that was clicked has a cached location, so the first frame
		// already has something to point at.
		SDK::FVector loc{};
		const bool locValid = ProductionBaseCore::GetBaseCoreLocation(baseKey, loc);

		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_active = true;
			++g_targetSerial;
			g_targetKey = baseKey;
			g_targetName = baseName;
			g_state = TargetState::Tracking;
			g_targetLocValid = locValid;
			g_targetLoc = loc;
		}

		LOG_DEBUG("ProductionWaypoint: SetTarget - now navigating to '%s' (Key=%llu)",
			baseName.c_str(), static_cast<unsigned long long>(baseKey));

		if (g_self && g_self->hooks->UI && g_widget)
			g_self->hooks->UI->SetWidgetVisible(g_widget, true);
	}

	void ClearTarget()
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_active)
				return;
			g_active = false;
		}

		LOG_DEBUG("ProductionWaypoint: ClearTarget");

		if (g_self && g_self->hooks->UI && g_widget)
			g_self->hooks->UI->SetWidgetVisible(g_widget, false);
	}
}

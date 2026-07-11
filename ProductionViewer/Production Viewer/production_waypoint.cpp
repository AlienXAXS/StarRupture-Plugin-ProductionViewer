#include "production_waypoint.h"
#include "production_basecore.h"
#include "Plugin Core/Helpers/plugin_helpers.h"

#include "Engine_classes.hpp"

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

		IPluginSelf* g_self = nullptr;
		WidgetHandle g_widget = nullptr;

		std::mutex g_mutex;
		bool g_active = false;
		uint64_t g_targetKey = 0;
		std::string g_targetName;

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
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				if (!g_active)
					return;
				targetKey = g_targetKey;
				targetName = g_targetName;
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

			SDK::UWorld* world = SDK::UWorld::GetWorld();
			SDK::APawn* pawn = world ? SDK::UGameplayStatics::GetPlayerPawn(world, 0) : nullptr;
			if (!pawn)
			{
				imgui->TextDisabled("Player unavailable.");
				LOG_TRACE("ProductionWaypoint: render - no player pawn (world=%p)", static_cast<void*>(world));
				return;
			}

			SDK::FVector targetLoc{};
			if (!ProductionBaseCore::GetBaseCoreLocation(targetKey, targetLoc))
			{
				imgui->TextDisabled("Location lost.");
				LOG_DEBUG("ProductionWaypoint: render - base core Key=%llu has no cached location - closing",
					static_cast<unsigned long long>(targetKey));
				ClearTarget();
				return;
			}

			const SDK::FVector playerLoc = pawn->K2_GetActorLocation();
			const double playerYaw = pawn->GetControlRotation().Yaw;

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

	void SetTarget(uint64_t baseKey, const std::string& baseName)
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_active = true;
			g_targetKey = baseKey;
			g_targetName = baseName;
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

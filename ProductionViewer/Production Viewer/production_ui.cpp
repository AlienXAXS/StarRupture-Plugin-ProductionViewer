#include "production_ui.h"
#include "production_data.h"
#include "plugin_helpers.h"
#include "plugin_config.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdio>
#include <string>

namespace ProductionUI
{
	namespace
	{
		// Mirrors ImGuiCol_ values from imgui.h so we don't need to depend on
		// the ImGui headers directly.
		constexpr int ImGuiCol_Button = 21;

		PanelHandle g_panel = nullptr;
		IPluginSelf* g_self = nullptr;
		bool g_panelOpen = false;

		int s_selectedRangeIndex = 1; // default to "1m"
		bool s_globalStatsProduction = false;
		bool s_globalStatsConsumption = false;

		std::string FormatAmount(float value)
		{
			char buf[32];
			if (value >= 1000000.0f)
				snprintf(buf, sizeof(buf), "%.1fM", value / 1000000.0f);
			else if (value >= 1000.0f)
				snprintf(buf, sizeof(buf), "%.1fk", value / 1000.0f);
			else
				snprintf(buf, sizeof(buf), "%.0f", value);
			return buf;
		}

		// Renders the scrollable item list: name, relative bar, running total, rate/min.
		void RenderEntryTable(IModLoaderImGui* imgui, const char* tableId, const std::vector<ProductionData::Entry>& entries)
		{
			if (entries.empty())
			{
				imgui->TextDisabled("No data yet.");
				return;
			}

			float maxTotal = 0.0f;
			for (const auto& entry : entries)
				maxTotal = (std::max)(maxTotal, entry.total);

			if (!imgui->BeginTable(tableId, 4, 0))
				return;

			imgui->TableSetupColumn("Item", 0, 0.0f);
			imgui->TableSetupColumn("", 0, 0.0f);
			imgui->TableSetupColumn("Total", 0, 0.0f);
			imgui->TableSetupColumn("Rate", 0, 0.0f);
			imgui->TableHeadersRow();

			for (const auto& entry : entries)
			{
				imgui->TableNextRow(0, 0.0f);

				imgui->TableNextColumn();
				imgui->Text(entry.name.c_str());

				imgui->TableNextColumn();
				float fraction = maxTotal > 0.0f ? entry.total / maxTotal : 0.0f;
				imgui->ProgressBar(fraction, -1.0f, 0.0f, "");

				imgui->TableNextColumn();
				imgui->Text(FormatAmount(entry.total).c_str());

				imgui->TableNextColumn();
				std::string rateText = FormatAmount(entry.ratePerMinute) + "/m";
				imgui->Text(rateText.c_str());
			}

			imgui->EndTable();
		}

		// Renders one side of the window (Production or Consumption): header,
		// aggregate history graph, and the item table.
		void RenderSidePanel(IModLoaderImGui* imgui, const char* childId, const char* title,
			const std::vector<ProductionData::Entry>& entries, bool* globalStats)
		{
			float availX, availY;
			imgui->GetContentRegionAvail(&availX, &availY);

			if (imgui->BeginChild(childId, availX, availY, true))
			{
				imgui->SeparatorText(title);
				imgui->Checkbox("Global statistics", globalStats);

				if (!entries.empty())
				{
					std::array<float, ProductionData::kHistorySamples> totals{};
					for (const auto& entry : entries)
						for (int i = 0; i < ProductionData::kHistorySamples; ++i)
							totals[i] += entry.history[i];

					float graphW, graphH;
					imgui->GetContentRegionAvail(&graphW, &graphH);
					imgui->PlotLines("", totals.data(), static_cast<int>(totals.size()), 0, nullptr, 0.0f, FLT_MAX, graphW, 120.0f);
				}
				else
				{
					imgui->TextDisabled("No graph data yet.");
				}

				imgui->Separator();

				std::string tableId = std::string(childId) + "_table";
				RenderEntryTable(imgui, tableId.c_str(), entries);
			}
			imgui->EndChild();
		}

		// Top-level render callback registered with the panel.
		void RenderProductionWindow(IModLoaderImGui* imgui)
		{
			// Time range selector
			const auto& ranges = ProductionData::GetTimeRanges();
			for (size_t i = 0; i < ranges.size(); ++i)
			{
				bool selected = static_cast<int>(i) == s_selectedRangeIndex;
				if (selected)
					imgui->PushStyleColor(ImGuiCol_Button, 0.85f, 0.55f, 0.10f, 1.0f);

				if (imgui->Button(ranges[i].label))
					s_selectedRangeIndex = static_cast<int>(i);

				if (selected)
					imgui->PopStyleColor(1);

				if (i + 1 < ranges.size())
					imgui->SameLine(0.0f, 4.0f);
			}

			imgui->Separator();

			// Category tabs (Items / Fluids / Buildings / Pollution / Kills)
			const auto& categories = ProductionData::GetCategories();
			if (imgui->BeginTabBar("ProductionViewer_Categories", 0))
			{
				for (const auto& category : categories)
				{
					if (imgui->BeginTabItem(category.name.c_str(), nullptr, 0))
					{
						if (imgui->BeginTable("ProductionViewer_Columns", 2, 0))
						{
							imgui->TableNextRow(0, 0.0f);

							imgui->TableNextColumn();
							RenderSidePanel(imgui, "ProductionViewer_ProductionPanel", "Production", category.production, &s_globalStatsProduction);

							imgui->TableNextColumn();
							RenderSidePanel(imgui, "ProductionViewer_ConsumptionPanel", "Consumption", category.consumption, &s_globalStatsConsumption);

							imgui->EndTable();
						}

						imgui->EndTabItem();
					}
				}
				imgui->EndTabBar();
			}
		}

		void OnPanelClosed(PanelHandle handle)
		{
			if (handle == g_panel)
				g_panelOpen = false;
		}

		void OnToggleKey(EModKey /*key*/, EModKeyEvent event)
		{
			if (event != EModKeyEvent::Pressed)
				return;

			if (!g_self || !g_self->hooks->UI || !g_panel)
				return;

			if (g_panelOpen)
				g_self->hooks->UI->SetPanelClose(g_panel);
			else
				g_self->hooks->UI->SetPanelOpen(g_panel);

			g_panelOpen = !g_panelOpen;
		}
	}

	void Init(IPluginSelf* self)
	{
		g_self = self;

		if (!self->hooks->UI)
		{
			LOG_WARN("UI hooks unavailable (server/generic build) - Production Viewer window not registered");
			return;
		}

		static PluginPanelDesc desc = { "Production Viewer", "Production Statistics", &RenderProductionWindow };
		g_panel = self->hooks->UI->RegisterPanel(&desc);

		self->hooks->UI->RegisterOnPanelWindowClosed(&OnPanelClosed);

		if (self->hooks->Input)
		{
			self->hooks->Input->RegisterKeybindByName(ProductionViewerConfig::Config::GetToggleKey(), EModKeyEvent::Pressed, &OnToggleKey);
		}

		LOG_INFO("Production Viewer window registered");
	}

	void Shutdown(IPluginSelf* self)
	{
		if (!self->hooks->UI)
		{
			g_self = nullptr;
			return;
		}

		if (self->hooks->Input)
		{
			self->hooks->Input->UnregisterKeybindByName(ProductionViewerConfig::Config::GetToggleKey(), EModKeyEvent::Pressed, &OnToggleKey);
		}

		self->hooks->UI->UnregisterOnPanelWindowClosed(&OnPanelClosed);

		if (g_panel)
		{
			self->hooks->UI->UnregisterPanel(g_panel);
			g_panel = nullptr;
		}

		g_panelOpen = false;
		g_self = nullptr;
	}
}

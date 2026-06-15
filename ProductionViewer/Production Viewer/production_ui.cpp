#include "production_ui.h"
#include "production_data.h"
#include "production_tracker.h"
#include "Plugin Core/Helpers/plugin_helpers.h"
#include "Plugin Core/Config/plugin_config.h"

#include "Engine_classes.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

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

		// Search bar text (comma-separated, matched against item names in both
		// the Production and Consumption columns).
		char s_searchBuffer[128] = "";

		// How often the item table/graphs are refreshed from ProductionTracker.
		// Rebuilding the snapshot (sorting, copying history vectors, etc.) every
		// frame is wasted work and makes the values jitter; once a second is
		// plenty for production statistics.
		constexpr std::chrono::milliseconds kRefreshInterval{1000};

		ProductionData::Category s_cachedItems;
		std::chrono::steady_clock::time_point s_lastRefreshTime{};
		int s_cachedRangeIndex = -1;

		// Returns a cached snapshot of the items for the selected range,
		// refreshing it at most once per kRefreshInterval (or immediately if the
		// selected range has changed).
		const ProductionData::Category& GetCachedItems(ProductionData::TimeRange range, int rangeIndex)
		{
			auto now = std::chrono::steady_clock::now();
			if (rangeIndex != s_cachedRangeIndex || now - s_lastRefreshTime >= kRefreshInterval)
			{
				s_cachedItems = ProductionTracker::GetItemsCategory(range);
				s_cachedRangeIndex = rangeIndex;
				s_lastRefreshTime = now;
			}

			return s_cachedItems;
		}

		// Splits the search box contents on commas into lowercase, trimmed
		// search terms. An item matches if its name contains any one of them.
		std::vector<std::string> ParseSearchTerms(const char* input)
		{
			std::vector<std::string> terms;
			std::string current;

			auto pushTerm = [&terms, &current]()
			{
				size_t begin = current.find_first_not_of(" \t");
				size_t end = current.find_last_not_of(" \t");
				if (begin != std::string::npos)
					terms.push_back(current.substr(begin, end - begin + 1));
				current.clear();
			};

			for (const char* p = input; *p; ++p)
			{
				if (*p == ',')
					pushTerm();
				else
					current += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
			}
			pushTerm();

			return terms;
		}

		// True if no search terms are set, or the name contains at least one of them.
		bool MatchesSearch(const std::string& name, const std::vector<std::string>& terms)
		{
			if (terms.empty())
				return true;

			std::string lowerName = name;
			std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });

			for (const std::string& term : terms)
				if (!term.empty() && lowerName.find(term) != std::string::npos)
					return true;

			return false;
		}

		// Returns the subset of entries whose name matches the search terms.
		std::vector<ProductionData::Entry> FilterEntries(const std::vector<ProductionData::Entry>& entries,
			const std::vector<std::string>& terms)
		{
			if (terms.empty())
				return entries;

			std::vector<ProductionData::Entry> filtered;
			for (const ProductionData::Entry& entry : entries)
				if (MatchesSearch(entry.name, terms))
					filtered.push_back(entry);

			return filtered;
		}

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

		// Renders the scrollable item list: name, history sparkline, running total, rate/min.
		void RenderEntryTable(IModLoaderImGui* imgui, const char* tableId, const std::vector<ProductionData::Entry>& entries)
		{
			if (entries.empty())
			{
				imgui->TextDisabled("No data yet.");
				return;
			}

			if (!imgui->BeginTable(tableId, 4, 0))
				return;

			constexpr int kColumnFlagsWidthStretch = 1 << 3; // ImGuiTableColumnFlags_WidthStretch
			constexpr int kColumnFlagsWidthFixed = 1 << 4;   // ImGuiTableColumnFlags_WidthFixed

			imgui->TableSetupColumn("Item", kColumnFlagsWidthStretch, 2.0f);
			imgui->TableSetupColumn("History", kColumnFlagsWidthStretch, 2.0f);
			imgui->TableSetupColumn("Total", kColumnFlagsWidthFixed, 60.0f);
			imgui->TableSetupColumn("Rate", kColumnFlagsWidthFixed, 60.0f);
			imgui->TableHeadersRow();

			IPluginImGuiTextures* textures = GetHooks() ? GetHooks()->ImGuiTextures : nullptr;
			constexpr float kIconSize = 20.0f;
			constexpr float kGraphHeight = 20.0f;

			for (const auto& entry : entries)
			{
				imgui->TableNextRow(0, 0.0f);

				imgui->TableNextColumn();
				if (entry.icon && textures)
				{
					textures->Image(entry.icon, kIconSize, kIconSize);
					imgui->SameLine(0.0f, 4.0f);
				}
				imgui->Text(entry.name.c_str());

				imgui->TableNextColumn();
				float graphW, graphH;
				imgui->GetContentRegionAvail(&graphW, &graphH);
				std::string graphId = "##history_" + entry.name;
				imgui->PlotLines(graphId.c_str(), entry.history.data(), static_cast<int>(entry.history.size()), 0, nullptr, 0.0f, FLT_MAX, graphW, kGraphHeight);

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
			const std::vector<ProductionData::Entry>& entries)
		{
			float availX, availY;
			imgui->GetContentRegionAvail(&availX, &availY);

			if (imgui->BeginChild(childId, availX, availY, true))
			{
				imgui->SeparatorText(title);

				if (!entries.empty())
				{
					std::array<float, ProductionData::kHistorySamples> totals{};
					for (const auto& entry : entries)
						for (int i = 0; i < ProductionData::kHistorySamples; ++i)
							totals[i] += entry.history[i];

					float graphW, graphH;
					imgui->GetContentRegionAvail(&graphW, &graphH);
					imgui->PlotLines("##graph", totals.data(), static_cast<int>(totals.size()), 0, nullptr, 0.0f, FLT_MAX, graphW, 120.0f);
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

			const auto& items = GetCachedItems(ranges[s_selectedRangeIndex].value, s_selectedRangeIndex);
			if (imgui->BeginTable("ProductionViewer_Columns", 2, 0))
			{
				imgui->TableNextRow(0, 0.0f);

				imgui->TableNextColumn();
				RenderSidePanel(imgui, "ProductionViewer_ProductionPanel", "Production", items.production);

				imgui->TableNextColumn();
				RenderSidePanel(imgui, "ProductionViewer_ConsumptionPanel", "Consumption", items.consumption);

				imgui->EndTable();
			}
		}

		void OnPanelClosed(PanelHandle handle)
		{
			LOG_DEBUG("ProductionUI: OnPanelClosed (handle=%p, ours=%s)", handle, handle == g_panel ? "yes" : "no");

			if (handle == g_panel)
				g_panelOpen = false;
		}

		void OnToggleKey(EModKey key, EModKeyEvent event)
		{
			LOG_DEBUG("ProductionUI: OnToggleKey (key=%u, event=%u)", static_cast<unsigned>(key), static_cast<unsigned>(event));

			if (event != EModKeyEvent::Pressed)
				return;

			if (!g_self || !g_self->hooks->UI || !g_panel)
			{
				LOG_DEBUG("ProductionUI: OnToggleKey ignored (self=%s, UI=%s, panel=%s)",
					g_self ? "yes" : "no",
					(g_self && g_self->hooks->UI) ? "yes" : "no",
					g_panel ? "yes" : "no");
				return;
			}

			if (g_panelOpen)
			{
				// OnPanelClosed will update g_panelOpen.
				g_self->hooks->UI->SetPanelClose(g_panel);
			}
			else
			{
				SDK::UWorld* world = SDK::UWorld::GetWorld();
				if (!world || world->GetName() != "ChimeraMain")
				{
					LOG_DEBUG("ProductionUI: OnToggleKey ignored (not in ChimeraMain map)");
					return;
				}

				g_self->hooks->UI->SetPanelOpen(g_panel);
				g_panelOpen = true;
			}

			LOG_DEBUG("ProductionUI: panel toggled (now %s)", g_panelOpen ? "open" : "closed");
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

		LOG_DEBUG("ProductionUI: RegisterPanel returned handle=%p", g_panel);

		self->hooks->UI->RegisterOnPanelWindowClosed(&OnPanelClosed);

		if (self->hooks->Input)
		{
			const char* toggleKey = ProductionViewerConfig::Config::GetToggleKey();
			LOG_DEBUG("ProductionUI: registering toggle keybind '%s'", toggleKey);
			self->hooks->Input->RegisterKeybindByName(toggleKey, EModKeyEvent::Pressed, &OnToggleKey);
		}
		else
		{
			LOG_DEBUG("ProductionUI: Input hooks unavailable - toggle keybind not registered");
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
			const char* toggleKey = ProductionViewerConfig::Config::GetToggleKey();
			LOG_DEBUG("ProductionUI: unregistering toggle keybind '%s'", toggleKey);
			self->hooks->Input->UnregisterKeybindByName(toggleKey, EModKeyEvent::Pressed, &OnToggleKey);
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

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
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
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

		int s_selectedRangeIndex = 0; // default to "1m"

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

		// Name of the item row hovered in each side panel's table on the
		// previous frame, used to highlight its line in the overview graph.
		// Keyed by the panel's childId.
		std::unordered_map<std::string, std::string> s_hoveredEntryByPanel;

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

		// Box moving-average over kSmoothing neighbours on each side.
		// Softens per-bucket spikes (e.g. a craft every 2s on 1s buckets)
		// without distorting longer trends.
		constexpr int kSmoothing = 2; // 5-bucket window
		std::array<float, ProductionData::kHistorySamples> SmoothHistory(
			const std::array<float, ProductionData::kHistorySamples>& in)
		{
			std::array<float, ProductionData::kHistorySamples> out{};
			for (int i = 0; i < ProductionData::kHistorySamples; ++i)
			{
				float sum = 0.0f;
				int count = 0;
				for (int j = i - kSmoothing; j <= i + kSmoothing; ++j)
				{
					if (j >= 0 && j < ProductionData::kHistorySamples)
					{
						sum += in[j];
						++count;
					}
				}
				out[i] = count > 0 ? sum / count : 0.0f;
			}
			return out;
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

		// ImGuiCol_PlotLines' numeric value has moved between ImGui versions
		// (e.g. tab-bar colors were inserted ahead of it), so resolve it by
		// name at runtime instead of hardcoding an index. Returns -1 if not found.
		int GetPlotLinesColorIndex(IModLoaderImGui* imgui)
		{
			static int s_index = -2;
			if (s_index == -2)
			{
				s_index = -1;
				for (int i = 0; i < 64; ++i)
				{
					const char* name = imgui->GetStyleColorName(i);
					if (name && std::strcmp(name, "PlotLines") == 0)
					{
						s_index = i;
						break;
					}
				}
			}
			return s_index;
		}

		// Label for the start (left edge) of the overview graph's x-axis,
		// describing how far back the selected time range reaches.
		const char* GetRangeStartLabel(ProductionData::TimeRange range)
		{
			switch (range)
			{
				case ProductionData::TimeRange::Minutes1:  return "-1m";
				case ProductionData::TimeRange::Minutes10: return "-10m";
				case ProductionData::TimeRange::Hours1:    return "-1h";
				case ProductionData::TimeRange::All:       return "oldest";
				default: return "";
			}
		}

		// Renders the scrollable item list: name, history sparkline, running total, rate/min.
		// Returns the name of the entry whose row is currently hovered, or an empty
		// string if none is.
		std::string RenderEntryTable(IModLoaderImGui* imgui, const char* tableId, const std::vector<ProductionData::Entry>& entries)
		{
			if (entries.empty())
			{
				imgui->TextDisabled("No data yet.");
				return {};
			}

			if (!imgui->BeginTable(tableId, 3, 0))
				return {};

			std::string hoveredName;

			constexpr int kColumnFlagsWidthStretch = 1 << 3; // ImGuiTableColumnFlags_WidthStretch
			constexpr int kColumnFlagsWidthFixed = 1 << 4;   // ImGuiTableColumnFlags_WidthFixed

			imgui->TableSetupColumn("Item", kColumnFlagsWidthStretch, 2.0f);
			imgui->TableSetupColumn("History", kColumnFlagsWidthStretch, 2.0f);
			imgui->TableSetupColumn("Rate", kColumnFlagsWidthFixed, 60.0f);
			imgui->TableHeadersRow();

			IPluginImGuiTextures* textures = GetHooks() ? GetHooks()->ImGuiTextures : nullptr;
			constexpr float kIconSize = 20.0f;
			constexpr float kGraphHeight = 20.0f;

			// ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap
			constexpr int kRowSelectableFlags = (1 << 1) | (1 << 4);
			constexpr int kTableBgTargetRowBg0 = 1;

			for (const auto& entry : entries)
			{
				imgui->TableNextRow(0, 0.0f);

				imgui->TableNextColumn();

				// Invisible, full-row selectable used purely for hover detection;
				// AllowOverlap lets the icon/text/graph below still render normally.
				std::string rowId = "##row_" + entry.name;
				imgui->SelectableFull(rowId.c_str(), false, kRowSelectableFlags, 0.0f, 0.0f);
				if (imgui->IsItemHovered())
				{
					hoveredName = entry.name;
					unsigned int highlightColor = imgui->GetColorU32FromVec4(1.0f, 1.0f, 1.0f, 0.08f);
					imgui->TableSetBgColor(kTableBgTargetRowBg0, highlightColor, -1);
				}
				imgui->SameLine(0.0f, 0.0f);

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
				auto smoothed = SmoothHistory(entry.historyRatePerMinute);
				imgui->PlotLines(graphId.c_str(), smoothed.data(), static_cast<int>(smoothed.size()), 0, nullptr, 0.0f, FLT_MAX, graphW, kGraphHeight);

				imgui->TableNextColumn();
				std::string rateText = FormatAmount(entry.ratePerMinute) + "/m";
				imgui->Text(rateText.c_str());
			}

			imgui->EndTable();
			return hoveredName;
		}

		// Renders one side of the window (Production or Consumption): header,
		// aggregate history graph, and the item table.
		void RenderSidePanel(IModLoaderImGui* imgui, const char* childId, const char* title,
			const std::vector<ProductionData::Entry>& entries, ProductionData::TimeRange range)
		{
			float availX, availY;
			imgui->GetContentRegionAvail(&availX, &availY);

			if (imgui->BeginChild(childId, availX, availY, true))
			{
				imgui->SeparatorText(title);

				// Entry highlighted by hovering its row in the table below, as of last
				// frame. Used to dim the other overview lines and brighten this one.
				const std::string& hoveredEntry = s_hoveredEntryByPanel[childId];

				if (!entries.empty())
				{
					// Pre-compute smoothed per-minute-rate history for every entry once;
					// reused for max-scale, plotting, and hover hit-testing.
					std::vector<std::array<float, ProductionData::kHistorySamples>> smoothed;
					smoothed.reserve(entries.size());
					for (const auto& entry : entries)
						smoothed.push_back(SmoothHistory(entry.historyRatePerMinute));

					// Shared scale across all overlaid lines so they're visually comparable.
					float maxValue = 0.0f;
					for (const auto& sh : smoothed)
						for (float v : sh)
							maxValue = (std::max)(maxValue, v);
					if (maxValue <= 0.0f)
						maxValue = 1.0f;

					float graphW, graphH;
					imgui->GetContentRegionAvail(&graphW, &graphH);
					constexpr float kOverviewGraphHeight = 150.0f;

					constexpr int kColFrameBg = 7;
					int colPlotLines = GetPlotLinesColorIndex(imgui);

					// Overlaid plot rect, captured from the first line so the rest can be
					// drawn at the same screen position (and so we can hit-test hover).
					float rectMinX = 0.0f, rectMinY = 0.0f, rectMaxX = 0.0f, rectMaxY = 0.0f;

					for (size_t i = 0; i < entries.size(); ++i)
					{
						const ProductionData::Entry& entry = entries[i];
						bool first = (i == 0);

						float hue = entries.size() > 1 ? static_cast<float>(i) / static_cast<float>(entries.size()) : 0.0f;

						// Dim every line except the one hovered in the table, which is
						// drawn brighter and fully opaque so it stands out.
						float value = 0.95f;
						float alpha = 1.0f;
						if (!hoveredEntry.empty())
						{
							bool isHovered = entry.name == hoveredEntry;
							value = isHovered ? 1.0f : 0.5f;
							alpha = isHovered ? 1.0f : 0.25f;
						}

						float r, g, b;
						imgui->ColorConvertHSVtoRGB(hue, 0.65f, value, &r, &g, &b);

						int pushedColors = 0;
						if (!first)
						{
							imgui->PushStyleColor(kColFrameBg, 0.0f, 0.0f, 0.0f, 0.0f);
							imgui->SetCursorScreenPos(rectMinX, rectMinY);
							++pushedColors;
						}
						if (colPlotLines >= 0)
						{
							imgui->PushStyleColor(colPlotLines, r, g, b, alpha);
							++pushedColors;
						}

						std::string graphId = "##overview_" + entry.name;
						imgui->PlotLines(graphId.c_str(), smoothed[i].data(), static_cast<int>(smoothed[i].size()),
							0, nullptr, 0.0f, maxValue, graphW, kOverviewGraphHeight);

						if (first)
						{
							imgui->GetItemRectMin(&rectMinX, &rectMinY);
							imgui->GetItemRectMax(&rectMaxX, &rectMaxY);
						}

						if (pushedColors > 0)
							imgui->PopStyleColor(pushedColors);
					}

					// Hover the overview graph to see which item a line belongs to.
					if (imgui->IsMouseHoveringRect(rectMinX, rectMinY, rectMaxX, rectMaxY, true))
					{
						float mouseX, mouseY;
						imgui->GetMousePos(&mouseX, &mouseY);

						float width = rectMaxX - rectMinX;
						float height = rectMaxY - rectMinY;
						if (width > 0.0f && height > 0.0f)
						{
							float xFrac = std::clamp((mouseX - rectMinX) / width, 0.0f, 1.0f);
							float yFrac = std::clamp((mouseY - rectMinY) / height, 0.0f, 1.0f);

							int sampleIndex = static_cast<int>(xFrac * (ProductionData::kHistorySamples - 1) + 0.5f);
							float valueAtMouse = maxValue * (1.0f - yFrac);

							const ProductionData::Entry* closest = nullptr;
							float closestDist = FLT_MAX;
							for (size_t ei = 0; ei < entries.size(); ++ei)
							{
								float dist = std::fabs(smoothed[ei][sampleIndex] - valueAtMouse);
								if (dist < closestDist)
								{
									closestDist = dist;
									closest = &entries[ei];
								}
							}

							if (closest)
								imgui->SetTooltip(closest->name.c_str());
						}
					}

					// Axis labels, overlaid on top of the graph corners.
					constexpr float kAxisLabelPadding = 4.0f;
					float textHeight = imgui->GetTextLineHeight();

					// Y-axis max, top-left.
					imgui->SetCursorScreenPos(rectMinX + kAxisLabelPadding, rectMinY + kAxisLabelPadding);
					imgui->TextDisabled(FormatAmount(maxValue).c_str());

					// X-axis range start, bottom-left.
					imgui->SetCursorScreenPos(rectMinX + kAxisLabelPadding, rectMaxY - textHeight - kAxisLabelPadding);
					imgui->TextDisabled(GetRangeStartLabel(range));

					// X-axis "now", bottom-right.
					const char* nowLabel = "now";
					float nowW, nowH;
					imgui->CalcTextSize(nowLabel, &nowW, &nowH, false, -1.0f);
					imgui->SetCursorScreenPos(rectMaxX - nowW - kAxisLabelPadding, rectMaxY - textHeight - kAxisLabelPadding);
					imgui->TextDisabled(nowLabel);
				}
				else
				{
					imgui->TextDisabled("No graph data yet.");
				}

				imgui->Separator();

				std::string tableId = std::string(childId) + "_table";
				s_hoveredEntryByPanel[childId] = RenderEntryTable(imgui, tableId.c_str(), entries);
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
			std::vector<std::string> searchTerms = ParseSearchTerms(s_searchBuffer);
			std::vector<ProductionData::Entry> production = FilterEntries(items.production, searchTerms);
			std::vector<ProductionData::Entry> consumption = FilterEntries(items.consumption, searchTerms);

			float availX, availY;
			imgui->GetContentRegionAvail(&availX, &availY);
			float searchBarHeight = imgui->GetFrameHeightWithSpacing();
			float contentHeight = availY - searchBarHeight;
			if (contentHeight < 0.0f)
				contentHeight = 0.0f;

			if (imgui->BeginChild("ProductionViewer_Content", availX, contentHeight, false))
			{
				if (imgui->BeginTable("ProductionViewer_Columns", 2, 0))
				{
					imgui->TableNextRow(0, 0.0f);

					imgui->TableNextColumn();
					RenderSidePanel(imgui, "ProductionViewer_ProductionPanel", "Production", production, ranges[s_selectedRangeIndex].value);

					imgui->TableNextColumn();
					RenderSidePanel(imgui, "ProductionViewer_ConsumptionPanel", "Consumption", consumption, ranges[s_selectedRangeIndex].value);

					imgui->EndTable();
				}
			}
			imgui->EndChild();

			imgui->Separator();
			imgui->SetNextItemWidth(-1.0f);
			imgui->InputTextWithHint("##ProductionViewer_Search", "Search items (comma-separated, e.g. wolf,tube,ore)",
				s_searchBuffer, sizeof(s_searchBuffer));
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

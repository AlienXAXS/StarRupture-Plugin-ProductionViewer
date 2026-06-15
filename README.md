# ProductionViewer -- StarRupture Plugin

A StarRupture plugin built on the StarRupture ModLoader that tracks every item your
factories produce and consume, and shows it in a live, searchable dashboard.

**Target:** Game client only

---

## What It Does

ProductionViewer adds a "Production Viewer" panel (toggle with `P` by default) with
two columns: **Production** and **Consumption**. Each column shows:

- An overlaid graph with one line per item, all sharing a common scale so they're
  directly comparable.
- Axis labels showing the current max value, the start of the selected time range,
  and "now".
- A scrollable table per item: icon, name, sparkline history, running total, and
  rate per minute.

### Features

- **Time ranges** -- switch between 5s / 1m / 10m / 1h / All Time. All Time totals
  persist across sessions (see Persistence below).
- **Search/filter** -- type comma-separated terms to filter items by name in both
  columns; the graph above updates to match.
- **Hover to highlight** -- hovering a row in the table highlights that item's
  line in the graph above (and dims the rest), and hovering the graph itself shows
  a tooltip naming the nearest line.
- **Crafting coverage** -- production/consumption is tracked both for crafters near
  the player (actor-based hook) and for Mass-simulated factories far from the
  player (Mass ECS signal hook), so totals stay accurate across the whole base.
- **Icons** -- item/recipe icons are pre-loaded on startup and shown next to each
  item name.

### Persistence

"All Time" totals are written to the active save's per-plugin data folder and
reloaded automatically when a save is loaded, so long-run totals survive game
restarts. The shorter rolling windows (5s/1m/10m/1h) intentionally reset each
session.

### Configuration

Configurable via the ModLoader config UI / config file:

| Section | Key        | Default | Description                              |
|---------|-----------|---------|-------------------------------------------|
| General | Enabled    | true    | Enable or disable the plugin              |
| Menu    | ToggleKey  | P       | Key to open / close the Production Viewer |

---

## Building

1. Clone this repo (with submodules, or set `GameSDKRoot` / `PluginSDKInclude` in `Shared.props`)
2. Open `StarRupture-Plugin-ProductionViewer.sln` in Visual Studio 2022
3. Build the `Client Release` configuration (x64)
4. The output DLL is written to `build\Client Release\Plugins\ProductionViewer.dll`
5. Drop it into `Binaries\Win64\Plugins\` alongside `dwmapi.dll` and launch the game

See [PluginDevelopment.md](PluginDevelopment.md) for the full plugin API reference.

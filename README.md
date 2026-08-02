# ProductionViewer -- StarRupture Plugin

A StarRupture plugin built on the StarRupture ModLoader that tracks every item your
factories produce and consume, and shows it in a live, searchable dashboard.

**Target:** Game client, plus a headless server build that feeds connected clients

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

### Multiplayer

Only the server sees the whole base. A client's Mass simulation covers whatever
the engine has streamed in around that player, so tracking locally on a client
produces numbers that quietly disagree with what the factories are actually
doing. The server is therefore the sole authority:

- **Solo** -- tracks locally, exactly as before. Nothing is sent anywhere.
- **Dedicated server** (`Server Release` build, dropped into the server's
  `Plugins\` folder) -- tracks the whole base and pushes the results to every
  connected client. It has no UI of its own.
- **Client** -- tracks nothing and hooks nothing. The panel shows only what the
  server sent, with a status line saying how fresh it is. If the server has no
  ProductionViewer installed, the panel says so instead of showing a
  half-truth.

The feed is incremental. Items and base cores are announced once under a small
numeric id; after that only the amounts crafted since the previous update go
over the wire, and anything that produced nothing in an interval is simply left
out -- an idle base costs a packet header every five seconds and nothing more.
Item **All Time** totals are replicated as absolute values whenever a client
joins or re-syncs, so a player who connects halfway through a session still
sees the base's real lifetime figures rather than starting from zero.

Clients also receive each base core's world position, so distances and the
"Go" waypoint work for them the same as for the host.

> **Listen hosts:** the ModLoader's network channel can only send to clients
> from a server build -- on a client build `SendPacketToAllClients` is a no-op.
> A listen host therefore still shows its *own* data correctly, but cannot feed
> anyone. The role handling already treats it as an authority, so it starts
> broadcasting for free if that ever changes.

### Persistence

"All Time" totals are written to the active save's per-plugin data folder and
reloaded automatically when a save is loaded, so long-run totals survive game
restarts. The shorter rolling windows (5s/1m/10m/1h) intentionally reset each
session.

### Configuration

Configurable via the ModLoader config UI / config file:

| Section | Key               | Default | Description                                                          |
|---------|-------------------|---------|----------------------------------------------------------------------|
| General | Enabled           | true    | Enable or disable the plugin                                          |
| Menu    | ToggleKey         | P       | Key to open / close the Production Viewer                             |
| Network | BroadcastInterval | 1.0     | Seconds between production updates sent from the server to clients    |
| Network | StaleAfterSeconds | 10.0    | Seconds without a server update before a client flags its data stale  |

---

## Building

1. Clone this repo (with submodules, or set `GameSDKRoot` / `PluginSDKInclude` in `Shared.props`)
2. Open `StarRupture-Plugin-ProductionViewer.sln` in Visual Studio 2022
3. Build the `Client Release` configuration (x64)
4. The output DLL is written to `build\Client Release\Plugins\ProductionViewer.dll`
5. Drop it into `Binaries\Win64\Plugins\` alongside `dwmapi.dll` and launch the game

For dedicated servers, build `Server Release` instead and drop
`build\Server Release\Plugins\ProductionViewer.dll` into the server's
`Plugins\` folder. The ModLoader refuses a DLL whose build target doesn't match
its own, so the two are not interchangeable — and both ends must be on the same
plugin version, or the packet schema check drops the feed and clients fall back
to "waiting for the server".

See [PluginDevelopment.md](PluginDevelopment.md) for the full plugin API reference.

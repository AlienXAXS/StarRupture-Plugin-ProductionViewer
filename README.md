# ProductionViewer -- StarRupture Plugin

A StarRupture plugin built on the StarRupture ModLoader that tracks every item your
factories produce and consume, and shows it in a live, searchable dashboard.

**Target:** Game client, plus a headless server build that feeds connected clients

**Requires:** ModLoader 1.25.0 or newer ([StarRupture-ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader))

On a dedicated server, keep the mod loader up to date. It carries the version object map rows for each server build, and on a server build it has no rows for, the plugin finds those values itself, as before.

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
- **Icons** -- item icons are shown next to each item name. They come from the
  loader's image library, and a row whose icon is still loading keeps its place.
  Item pictures are drawn bigger and sharper than before.
  Ammo and the blueprint items are tracked as separate items, each with its own icon.
  The old combined row from an earlier version is removed from saved sessions, so these
  items count again from zero.
- **Game updates** -- the sizes of the base core query results, item stacks and the item name
  helper come from the loader's version map, so a game update that changes them
  no longer breaks tracking.

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

The Go overlay keeps showing the base's current name, and a rename shows within a few seconds. Its arrow turns with where the camera looks (in the drone or a vehicle too), and a distance over 1000 m is shown in km. A demolished base core is dropped from the list on its own within a few seconds, for clients on a server too, and the overlay closes. After you load a save or travel, the overlay finds the base again by its position. If it can't, it shows "Location lost." and closes after about a minute.

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

Put the plugin next to `StarRupture-Plugin-SDK` and `StarRupture-Game-SDK`:

```
git clone https://github.com/AlienXAXS/StarRupture-Game-SDK.git
git clone https://github.com/AlienXAXS/StarRupture-Plugin-SDK.git
git clone https://github.com/AlienXAXS/StarRupture-Plugin-ProductionViewer.git
```

```
<parent folder>/
├── StarRupture-Game-SDK/
├── StarRupture-Plugin-SDK/
└── StarRupture-Plugin-ProductionViewer/
```

To build against other folders, pass `/p:PluginSDKInclude=...` and `/p:GameSDKRoot=...` to msbuild.

Then:

1. Open `StarRupture-Plugin-ProductionViewer.sln` in Visual Studio 2022
2. Build the `Client Release` configuration (x64)
3. The output DLL is written to `build\Client Release\Plugins\ProductionViewer.dll`
4. Drop it into `Binaries\Win64\Plugins\` alongside `dwmapi.dll` and launch the game

For dedicated servers, build `Server Release` instead and drop
`build\Server Release\Plugins\ProductionViewer.dll` into the server's
`Plugins\` folder. The ModLoader refuses a DLL whose build target doesn't match
its own, so the two are not interchangeable — and both ends must be on the same
plugin version, or the packet schema check drops the feed and clients fall back
to "waiting for the server".

See [PluginDevelopment.md](PluginDevelopment.md) for the full plugin API reference.

## Reporting Bugs

Found a bug? [Report it on GitHub](https://github.com/AlienXAXS/StarRupture-Plugin-ProductionViewer/issues) and attach your `ModLoader.log` (in `<game>\StarRupture\Binaries\Win64\ModLoader\Logs\`).

## A server can switch parts off (not active yet)

A server host will be able to switch parts of Production Viewer off for the whole server, and back on, without a restart. The parts are:

- `tracking`: counting what factories craft. Off means nothing is counted, shown or sent.
- `basecores`: the split by base and the Go waypoint. Off means item rows have no per-base breakdown and no Go button, and an open Go overlay closes. Totals keep counting.
- `sessionsave`: saving and loading All Time totals with the save. Off means totals don't carry over and nothing is written to the session.
- `icons`: item pictures. Off means rows are drawn without them.

A part that is off stops drawing, sending, applying and writing. Nothing is written to the game, so there is nothing to undo. Turning it back on brings it back.

On a client, switching `tracking` off shows "No update from the server recently." (or "Waiting for production data from the server...") instead of the tables.

This is not active yet: it needs a mod loader release with host rules, which is not out. Until then nothing changes. With a mod loader that has it, the host runs `features hostoff ProductionViewer <id>` in the server console, for example `features hostoff ProductionViewer icons`, and `features hostclear` to lift it.

## Credits

Original mod by [@AlienXAXS](https://github.com/AlienXAXS).

Maintained by [@msantoro12](https://github.com/msantoro12).

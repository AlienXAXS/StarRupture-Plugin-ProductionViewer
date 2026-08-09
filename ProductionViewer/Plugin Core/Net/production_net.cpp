#include "production_net.h"
#include "production_packets.h"
#include "Plugin Core/Helpers/plugin_helpers.h"
#include "Plugin Core/Config/plugin_config.h"
#include "Production Viewer/production_tracker.h"

#include "plugin_network_helpers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ProductionNet
{
	namespace
	{
		// How long a client waits between nudging a silent server. Long enough
		// not to spam a vanilla server that will never answer, short enough that
		// a server which hot-loaded the plugin after we joined gets picked up.
		constexpr float kHelloRetrySeconds = 10.0f;

		// A server with nothing being crafted would otherwise go silent and read
		// as a dead link. An empty delta packet at this cadence is the liveness
		// signal; it costs a header and nothing else.
		constexpr float kHeartbeatSeconds = 5.0f;

		double NowSeconds()
		{
			using namespace std::chrono;
			return duration<double>(steady_clock::now().time_since_epoch()).count();
		}

		// Both tunables are consulted every tick, so they're cached rather than
		// re-read from the config each frame. The refresh keeps a live edit in
		// the ModLoader's config UI taking effect without a restart.
		constexpr float kConfigRefreshSeconds = 5.0f;
		float g_configRefreshTimer = kConfigRefreshSeconds;
		float g_broadcastInterval  = 1.0f;
		float g_staleAfterSeconds  = 10.0f;
		float g_fullSyncInterval   = 10.0f;

		void RefreshTunables(float deltaSeconds)
		{
			g_configRefreshTimer += deltaSeconds;
			if (g_configRefreshTimer < kConfigRefreshSeconds)
				return;

			g_configRefreshTimer = 0.0f;
			g_broadcastInterval  = ProductionViewerConfig::Config::GetBroadcastInterval();
			g_staleAfterSeconds  = ProductionViewerConfig::Config::GetStaleAfterSeconds();
			g_fullSyncInterval   = ProductionViewerConfig::Config::GetFullSyncInterval();
		}

		// ---- Shared -------------------------------------------------------

		// Read from the render thread (the panel) and from whatever thread the
		// Mass crafting signal lands on, so these are atomics rather than
		// mutex-guarded state.
		std::atomic<SessionRole> g_role{ SessionRole::Unresolved };
		std::atomic<uint8_t>     g_waitReason{ static_cast<uint8_t>(WaitReason::DetectingSession) };
		std::atomic<double>      g_lastPacketTime{ 0.0 };
		std::atomic<bool>        g_haveServerData{ false };

		PluginNetworkMessageCallback       g_itemDefHandler   = nullptr;
		PluginNetworkMessageCallback       g_baseDefHandler   = nullptr;
		PluginNetworkMessageCallback       g_deltaHandler     = nullptr;
		PluginNetworkMessageCallback       g_baseStatsHandler = nullptr;
		PluginNetworkMessageCallback       g_resetHandler     = nullptr;
		PluginNetworkServerMessageCallback g_helloHandler     = nullptr;

		// The loader's client-ready callback no-ops (and warns) unless this
		// process currently holds net authority, and nothing holds authority at
		// PluginInit - there is no world yet to ask. So it is registered from the
		// tick, the moment the role resolves to one that has clients. Registering
		// late costs nothing: the callback fires immediately for every client
		// already ready.
		bool g_clientReadyRegistered = false;

		// ---- Authority side ----------------------------------------------

		// Guards the dictionaries and the pending buffer. OnLocalSample can be
		// called from a Mass worker thread; the flush runs on the game thread.
		std::mutex g_authorityMutex;

		struct ItemDictEntry
		{
			uint16_t    id = 0;
			std::string displayName;
			bool        defSent = false;
		};

		struct BaseDictEntry
		{
			uint16_t    id = 0;
			std::string name;
			bool        hasLocation = false;
			float       locX = 0.0f, locY = 0.0f, locZ = 0.0f;
			bool        defDirty = true;   // needs (re)announcing: new, renamed, or newly located
		};

		struct PendingBase
		{
			uint16_t id = 0;
			float    produced = 0.0f;
			float    consumed = 0.0f;
		};

		struct PendingItem
		{
			uint16_t id = 0;
			float    produced = 0.0f;
			float    consumed = 0.0f;
			std::map<uint64_t, PendingBase> bases;   // keyed by packed base core handle
		};

		std::unordered_map<std::string, ItemDictEntry> g_itemDict;   // item key -> dictionary entry
		std::unordered_map<uint64_t, BaseDictEntry>    g_baseDict;   // packed base handle -> dictionary entry
		std::unordered_map<std::string, PendingItem>   g_pending;    // item key -> unsent activity
		uint16_t g_nextItemId = 0;
		uint16_t g_nextBaseId = 0;

		uint32_t g_generation      = 1;
		uint32_t g_sequence        = 0;
		float    g_broadcastAccum  = 0.0f;
		float    g_sinceLastSend   = 0.0f;
		float    g_fullSyncAccum   = 0.0f;

		// Players waiting on a full snapshot. Serviced (and cleared) on the next
		// tick so a controller that disconnected in the meantime is never held
		// for more than a frame.
		std::vector<void*> g_snapshotRequests;
		std::mutex         g_snapshotMutex;

		// ---- Client side --------------------------------------------------
		// Touched only from the game thread: the packet handlers and Tick.

		std::unordered_map<uint16_t, std::string> g_remoteItemKeys;
		struct RemoteBase { uint64_t key = 0; std::string name; };
		std::unordered_map<uint16_t, RemoteBase> g_remoteBases;

		uint32_t g_remoteGeneration = 0;
		uint32_t g_lastSequence     = 0;
		bool     g_haveSequence     = false;
		float    g_helloCooldown    = 0.0f;
		bool     g_helloPending     = true;

		// Set from the loader's server-ready callback, which carries no promise
		// about which thread it lands on; the tick consumes it.
		std::atomic<bool> g_serverReadySignal{ false };

		// ---- Small helpers ------------------------------------------------

		void CopyFixed(char* dst, int capacity, const std::string& src)
		{
			strncpy_s(dst, static_cast<size_t>(capacity), src.c_str(), _TRUNCATE);
		}

		// Packet strings are truncated with _TRUNCATE on the way out, but a
		// mismatched or corrupted sender could still hand us an unterminated
		// array - bound the read by the field, never by the terminator.
		std::string ReadFixed(const char* src, int capacity)
		{
			const size_t length = strnlen(src, static_cast<size_t>(capacity));
			return std::string(src, length);
		}

		SessionRole RoleFromNetMode(EPluginNetMode mode)
		{
			switch (mode)
			{
				case EPluginNetMode::Standalone:      return SessionRole::Solo;
				case EPluginNetMode::ListenServer:    return SessionRole::ListenHost;
				case EPluginNetMode::DedicatedServer: return SessionRole::DedicatedServer;
				case EPluginNetMode::Client:          return SessionRole::RemoteClient;
				default:                              return SessionRole::Unresolved;
			}
		}

		void SetWaitReason(WaitReason reason)
		{
			g_waitReason.store(static_cast<uint8_t>(reason), std::memory_order_relaxed);
		}

		// ---- Authority: outgoing -------------------------------------------

		struct ItemDefOut
		{
			uint16_t    id;
			std::string key;
			std::string displayName;

			// All Time totals as of the moment this def was built. These are what
			// make a client's numbers match the server's: the delta stream alone
			// would only ever describe the time since the client connected.
			float produced = 0.0f;
			float consumed = 0.0f;
			float elapsed  = 0.0f;
		};

		struct BaseDefOut
		{
			uint16_t    id;
			uint64_t    key;
			std::string name;
			bool        hasLocation;
			float       locX, locY, locZ;
		};

		// Reads the current totals into a batch of defs. Deliberately a separate
		// pass: the tracker's lock must never be taken while g_authorityMutex is
		// held, because a craft completion locks them the other way round.
		void FillItemDefTotals(std::vector<ItemDefOut>& defs)
		{
			for (ItemDefOut& def : defs)
				ProductionTracker::GetAllTimeTotals(def.key, def.produced, def.consumed, def.elapsed);
		}

		void FillItemDefEntry(PvItemDefPacket::Entry& entry, const ItemDefOut& def)
		{
			entry = {};
			entry.itemId          = def.id;
			entry.allTimeProduced = def.produced;
			entry.allTimeConsumed = def.consumed;
			entry.allTimeElapsed  = def.elapsed;
			CopyFixed(entry.key, kItemKeyChars, def.key);
			CopyFixed(entry.displayName, kItemNameChars, def.displayName);
		}

		void FillBaseDefEntry(PvBaseDefPacket::Entry& entry, const BaseDefOut& def)
		{
			entry = {};
			entry.baseId      = def.id;
			entry.hasLocation = def.hasLocation ? 1 : 0;
			entry.baseKey     = def.key;
			entry.locX        = def.locX;
			entry.locY        = def.locY;
			entry.locZ        = def.locZ;
			CopyFixed(entry.name, kBaseNameChars, def.name);
		}

		// `send` decides broadcast vs unicast, so the same packing code serves
		// both the periodic feed and a joining player's snapshot.
		template<typename SendFn>
		void SendItemDefs(const std::vector<ItemDefOut>& defs, SendFn&& send)
		{
			for (size_t i = 0; i < defs.size(); i += kItemDefsPerPacket)
			{
				PvItemDefPacket packet{};
				packet.schemaVersion = kSchemaVersion;
				packet.generation    = g_generation;

				const size_t chunk = (std::min)(static_cast<size_t>(kItemDefsPerPacket), defs.size() - i);
				for (size_t j = 0; j < chunk; ++j)
					FillItemDefEntry(packet.entries[j], defs[i + j]);
				packet.count = static_cast<uint8_t>(chunk);

				send(packet);
			}
		}

		template<typename SendFn>
		void SendBaseDefs(const std::vector<BaseDefOut>& defs, SendFn&& send)
		{
			for (size_t i = 0; i < defs.size(); i += kBaseDefsPerPacket)
			{
				PvBaseDefPacket packet{};
				packet.schemaVersion = kSchemaVersion;
				packet.generation    = g_generation;

				const size_t chunk = (std::min)(static_cast<size_t>(kBaseDefsPerPacket), defs.size() - i);
				for (size_t j = 0; j < chunk; ++j)
					FillBaseDefEntry(packet.entries[j], defs[i + j]);
				packet.count = static_cast<uint8_t>(chunk);

				send(packet);
			}
		}

		template<typename SendFn>
		void SendBaseStats(const std::vector<PvBaseStatsPacket::Entry>& stats, SendFn&& send)
		{
			for (size_t i = 0; i < stats.size(); i += kBaseStatsPerPacket)
			{
				PvBaseStatsPacket packet{};
				packet.schemaVersion = kSchemaVersion;
				packet.generation    = g_generation;

				const size_t chunk = (std::min)(static_cast<size_t>(kBaseStatsPerPacket), stats.size() - i);
				for (size_t j = 0; j < chunk; ++j)
					packet.entries[j] = stats[i + j];
				packet.count = static_cast<uint8_t>(chunk);

				send(packet);
			}
		}

		template<typename T>
		void Broadcast(const T& packet)
		{
			IPluginSelf* self = GetSelf();
			IPluginHooks* hooks = GetHooks();
			if (!self || !hooks || !hooks->Network) return;

			Network::SendPacketToAllClients(hooks, self, packet);
			g_sinceLastSend = 0.0f;
		}

		// Moves everything that has accumulated since the last broadcast onto
		// the wire: dictionary entries first (a delta referring to an id the
		// client hasn't been told about is worthless), then the deltas.
		void FlushAuthority()
		{
			std::unordered_map<std::string, PendingItem> pending;
			std::vector<ItemDefOut> itemDefs;
			std::vector<BaseDefOut> baseDefs;

			{
				std::lock_guard<std::mutex> lock(g_authorityMutex);
				pending.swap(g_pending);

				for (auto& [key, entry] : g_itemDict)
				{
					if (entry.defSent)
						continue;
					itemDefs.push_back(ItemDefOut{ entry.id, key, entry.displayName });
					entry.defSent = true;
				}

				for (auto& [baseKey, entry] : g_baseDict)
				{
					if (!entry.defDirty)
						continue;
					baseDefs.push_back(BaseDefOut{ entry.id, baseKey, entry.name,
						entry.hasLocation, entry.locX, entry.locY, entry.locZ });
					entry.defDirty = false;
				}
			}

			FillItemDefTotals(itemDefs);

			auto broadcast = [](const auto& packet) { Broadcast(packet); };
			SendItemDefs(itemDefs, broadcast);
			SendBaseDefs(baseDefs, broadcast);

			// An item announced in this same flush carries its All Time totals
			// in the def, and those totals already include this interval's
			// crafts - sending the item-level delta too would count them twice.
			// The cost is that the item's rolling 1m window misses its very
			// first interval, which is the right side of that trade: a wrong
			// lifetime total is permanent, a wrong first second is not. The
			// per-base figures aren't in the def, so they still go out.
			std::unordered_set<std::string> definedNow;
			definedNow.reserve(itemDefs.size());
			for (const ItemDefOut& def : itemDefs)
				definedNow.insert(def.key);

			std::vector<PvDeltaPacket::Entry> entries;
			for (const auto& [itemKey, item] : pending)
			{
				if ((item.produced > 0.0f || item.consumed > 0.0f) && definedNow.count(itemKey) == 0)
					entries.push_back(PvDeltaPacket::Entry{ item.id, kBaseIdItemTotal, item.produced, item.consumed });

				for (const auto& [baseKey, base] : item.bases)
					entries.push_back(PvDeltaPacket::Entry{ item.id, base.id, base.produced, base.consumed });
			}

			if (entries.empty())
			{
				// Nothing happened. Stay quiet unless the link needs a sign of
				// life, so an idle base costs no bandwidth at all.
				if (g_sinceLastSend < kHeartbeatSeconds)
					return;

				PvDeltaPacket packet{};
				packet.schemaVersion = kSchemaVersion;
				packet.generation    = g_generation;
				packet.sequence      = ++g_sequence;
				packet.count         = 0;
				Broadcast(packet);
				return;
			}

			for (size_t i = 0; i < entries.size(); i += kDeltasPerPacket)
			{
				PvDeltaPacket packet{};
				packet.schemaVersion = kSchemaVersion;
				packet.generation    = g_generation;
				packet.sequence      = ++g_sequence;

				const size_t chunk = (std::min)(static_cast<size_t>(kDeltasPerPacket), entries.size() - i);
				for (size_t j = 0; j < chunk; ++j)
					packet.entries[j] = entries[i + j];
				packet.count = static_cast<uint8_t>(chunk);

				Broadcast(packet);
			}
		}

		// ---- Authority: full state -----------------------------------------

		// Everything this authority holds, ready for the wire. Absolute figures
		// throughout, and the client applies every one of them by overwrite, so
		// sending this is safe whatever the receiver currently believes.
		struct FullState
		{
			std::vector<ItemDefOut>              itemDefs;
			std::vector<BaseDefOut>              baseDefs;
			std::vector<PvBaseStatsPacket::Entry> baseStats;
		};

		// Interns anything the tracker knows that the dictionaries have not seen,
		// then packs the lot. The interning matters as much as the packing: the
		// dictionaries are only ever fed by OnLocalSample, so an item restored
		// from the save whose production has since stopped has no id, is in no
		// delta, and would never reach a client at all.
		//
		// `markAnnounced` clears the per-entry dirty flags and so may only be set
		// when the result is going to every client. A unicast snapshot must leave
		// them alone, or the clients it was not addressed to would never be told
		// about the definitions it happened to intern.
		void BuildFullState(FullState& out, bool markAnnounced)
		{
			// Read before g_authorityMutex is taken, never underneath it: a craft
			// completion locks the tracker first and this module second, so the
			// opposite order here would eventually deadlock.
			std::vector<ProductionTracker::ItemTotals> items;
			std::vector<ProductionTracker::BaseTotals> bases;
			ProductionTracker::GetReplicationSnapshot(items, bases);

			out.itemDefs.clear();
			out.baseDefs.clear();
			out.baseStats.clear();

			std::lock_guard<std::mutex> lock(g_authorityMutex);

			out.itemDefs.reserve(items.size());
			out.baseStats.reserve(bases.size());

			std::unordered_map<std::string, uint16_t> itemIds;
			itemIds.reserve(items.size());

			for (const ProductionTracker::ItemTotals& item : items)
			{
				if (item.produced <= 0.0f && item.consumed <= 0.0f)
					continue;   // nothing the panel would draw; not worth an id

				auto [itemIt, itemIsNew] = g_itemDict.try_emplace(item.key);
				ItemDictEntry& dict = itemIt->second;
				if (itemIsNew)
				{
					if (g_nextItemId >= kBaseIdItemTotal)
					{
						g_itemDict.erase(itemIt);
						continue;
					}
					dict.id = g_nextItemId++;
					dict.displayName = item.displayName;
				}
				if (markAnnounced)
					dict.defSent = true;

				itemIds[item.key] = dict.id;

				ItemDefOut def;
				def.id          = dict.id;
				def.key         = item.key;
				def.displayName = dict.displayName;
				def.produced    = item.produced;
				def.consumed    = item.consumed;
				def.elapsed     = item.elapsedSeconds;
				out.itemDefs.push_back(std::move(def));
			}

			for (const ProductionTracker::BaseTotals& base : bases)
			{
				auto itemIdIt = itemIds.find(base.key);
				if (itemIdIt == itemIds.end())
					continue;   // its item was skipped, so this row has nowhere to hang

				if (base.produced <= 0.0f && base.consumed <= 0.0f)
					continue;

				auto [baseIt, baseIsNew] = g_baseDict.try_emplace(base.baseKey);
				BaseDictEntry& dict = baseIt->second;
				if (baseIsNew)
				{
					if (g_nextBaseId >= kBaseIdItemTotal)
					{
						g_baseDict.erase(baseIt);
						continue;
					}
					dict.id = g_nextBaseId++;
				}

				// Marked dirty, not just updated: when this state is only going to
				// one player, the incremental broadcast is still how everyone
				// else hears about it. The final pass below clears the flag again
				// when the state is going to all of them anyway.
				if (dict.name != base.baseName)
				{
					dict.name = base.baseName;
					dict.defDirty = true;
				}

				if (base.hasLocation && !dict.hasLocation)
				{
					dict.hasLocation = true;
					dict.locX = base.locX;
					dict.locY = base.locY;
					dict.locZ = base.locZ;
					dict.defDirty = true;
				}

				out.baseStats.push_back(PvBaseStatsPacket::Entry{
					itemIdIt->second, dict.id, base.produced, base.consumed });
			}

			// Every base, not just the ones touched above: a stats row is useless
			// to a client that cannot resolve its id, and this is the packet that
			// carries the name and location.
			out.baseDefs.reserve(g_baseDict.size());
			for (auto& [baseKey, entry] : g_baseDict)
			{
				out.baseDefs.push_back(BaseDefOut{ entry.id, baseKey, entry.name,
					entry.hasLocation, entry.locX, entry.locY, entry.locZ });
				if (markAnnounced)
					entry.defDirty = false;
			}
		}

		// The full state, to everyone. This is what bounds the error in the feed
		// rather than letting it accumulate: a def packet lost in the join window,
		// a delta that went missing, an item that stopped being produced before
		// this client ever connected - all of it is repaired here, on a fixed
		// cadence, without either end having to notice anything went wrong.
		void BroadcastFullSync(const FullState& state)
		{
			auto broadcast = [](const auto& packet) { Broadcast(packet); };
			SendItemDefs(state.itemDefs, broadcast);
			SendBaseDefs(state.baseDefs, broadcast);
			SendBaseStats(state.baseStats, broadcast);
		}

		// Sends one player the whole state. Always run straight after
		// FlushAuthority so the totals it carries are strictly newer than every
		// delta already on the wire - otherwise a delta sent this interval could
		// land after the snapshot and be counted twice.
		void ServiceSnapshotRequests(const FullState& state)
		{
			std::vector<void*> requests;
			{
				std::lock_guard<std::mutex> lock(g_snapshotMutex);
				if (g_snapshotRequests.empty())
					return;
				requests.swap(g_snapshotRequests);
			}

			IPluginSelf* self = GetSelf();
			IPluginHooks* hooks = GetHooks();
			if (!self || !hooks || !hooks->Network)
				return;

			for (void* controller : requests)
			{
				if (!controller)
					continue;

				// The reset tells the client to drop whatever it had before it
				// starts adopting these definitions, so a resync after a
				// dropped packet clears the stale ids too.
				PvResetPacket reset{};
				reset.schemaVersion = kSchemaVersion;
				reset.generation    = g_generation;
				Network::SendPacketToPlayer(hooks, self, controller, reset);

				auto send = [hooks, self, controller](const auto& packet)
				{
					Network::SendPacketToPlayer(hooks, self, controller, packet);
				};
				SendItemDefs(state.itemDefs, send);
				SendBaseDefs(state.baseDefs, send);
				SendBaseStats(state.baseStats, send);

				// An empty delta hands over the current sequence number, so the
				// client's gap detection starts from the right place instead of
				// firing on the next real packet.
				PvDeltaPacket sync{};
				sync.schemaVersion = kSchemaVersion;
				sync.generation    = g_generation;
				sync.sequence      = g_sequence;
				sync.count         = 0;
				Network::SendPacketToPlayer(hooks, self, controller, sync);
			}

			LOG_DEBUG("ProductionNet: sent snapshot (%zu item(s), %zu base(s), %zu base total(s)) to %zu player(s)",
				state.itemDefs.size(), state.baseDefs.size(), state.baseStats.size(), requests.size());
		}

		// ---- Client: incoming ----------------------------------------------

		void RequestResync(const char* why)
		{
			g_helloPending = true;
			LOG_DEBUG("ProductionNet: requesting resync from the server (%s)", why);
		}

		void WipeRemoteState()
		{
			g_remoteItemKeys.clear();
			g_remoteBases.clear();
			g_lastSequence = 0;
			g_haveSequence = false;
			ProductionTracker::Clear();
		}

		// Adopts a new generation, discarding everything from the world the
		// server has just left. Returns false if the packet should be ignored.
		bool AcceptGeneration(uint32_t generation, bool packetCarriesState)
		{
			if (generation == g_remoteGeneration)
				return true;

			LOG_INFO("ProductionNet: server generation changed (%u -> %u) - dropping cached production data",
				g_remoteGeneration, generation);
			WipeRemoteState();
			g_remoteGeneration = generation;

			if (!packetCarriesState)
			{
				RequestResync("generation changed");
				return false;
			}
			return true;
		}

		void MarkPacketReceived()
		{
			g_lastPacketTime.store(NowSeconds(), std::memory_order_relaxed);
			g_haveServerData.store(true, std::memory_order_relaxed);
			SetWaitReason(WaitReason::None);
		}

		void OnItemDefPacket(const PvItemDefPacket& packet)
		{
			if (packet.schemaVersion != kSchemaVersion)
			{
				LOG_WARN("ProductionNet: dropped item definitions with schema %u (expected %u) - "
					"the server is running a different plugin version",
					packet.schemaVersion, kSchemaVersion);
				return;
			}
			if (!AcceptGeneration(packet.generation, true))
				return;

			const int count = (std::min)(static_cast<int>(packet.count), kItemDefsPerPacket);
			for (int i = 0; i < count; ++i)
			{
				const PvItemDefPacket::Entry& entry = packet.entries[i];
				std::string key = ReadFixed(entry.key, kItemKeyChars);
				if (key.empty())
					continue;

				std::string displayName = ReadFixed(entry.displayName, kItemNameChars);
				if (displayName.empty())
					displayName = key;

				g_remoteItemKeys[entry.itemId] = key;
				ProductionTracker::SeedRemoteItem(key, displayName,
					entry.allTimeProduced, entry.allTimeConsumed, entry.allTimeElapsed);
			}

			MarkPacketReceived();
		}

		void OnBaseDefPacket(const PvBaseDefPacket& packet)
		{
			if (packet.schemaVersion != kSchemaVersion)
				return;
			if (!AcceptGeneration(packet.generation, true))
				return;

			const int count = (std::min)(static_cast<int>(packet.count), kBaseDefsPerPacket);
			for (int i = 0; i < count; ++i)
			{
				const PvBaseDefPacket::Entry& entry = packet.entries[i];

				RemoteBase& base = g_remoteBases[entry.baseId];
				base.key  = entry.baseKey;
				base.name = ReadFixed(entry.name, kBaseNameChars);

				// The server's world position lets this client show distances
				// and drive the "Go" waypoint. It cannot resolve the base core
				// itself: Mass entity handles are local to one process.
				if (entry.hasLocation && entry.baseKey != 0)
					ProductionTracker::SetRemoteBaseLocation(entry.baseKey, entry.locX, entry.locY, entry.locZ);
			}

			MarkPacketReceived();
		}

		void OnDeltaPacket(const PvDeltaPacket& packet)
		{
			if (packet.schemaVersion != kSchemaVersion)
			{
				LOG_WARN("ProductionNet: dropped a delta with schema %u (expected %u) - "
					"the server is running a different plugin version",
					packet.schemaVersion, kSchemaVersion);
				return;
			}
			if (!AcceptGeneration(packet.generation, false))
				return;

			// A gap means a packet went missing, and deltas do not self-heal -
			// ask for a fresh snapshot, which overwrites the totals outright.
			if (g_haveSequence && packet.sequence != g_lastSequence + 1)
			{
				if (packet.sequence > g_lastSequence)
					RequestResync("sequence gap");
			}
			g_lastSequence = packet.sequence;
			g_haveSequence = true;

			const int count = (std::min)(static_cast<int>(packet.count), kDeltasPerPacket);
			for (int i = 0; i < count; ++i)
			{
				const PvDeltaPacket::Entry& entry = packet.entries[i];

				auto itemIt = g_remoteItemKeys.find(entry.itemId);
				if (itemIt == g_remoteItemKeys.end())
					continue;   // definition lost or still in flight; the next full sync covers it

				if (entry.baseId == kBaseIdItemTotal)
				{
					ProductionTracker::ApplyRemoteItemDelta(itemIt->second, entry.produced, entry.consumed);
					continue;
				}

				auto baseIt = g_remoteBases.find(entry.baseId);
				if (baseIt == g_remoteBases.end())
					continue;

				ProductionTracker::ApplyRemoteBaseDelta(itemIt->second, baseIt->second.key,
					baseIt->second.name, entry.produced, entry.consumed);
			}

			MarkPacketReceived();
		}

		void OnBaseStatsPacket(const PvBaseStatsPacket& packet)
		{
			if (packet.schemaVersion != kSchemaVersion)
				return;

			// Unlike the def packets this one carries no strings, so it cannot
			// stand on its own after a wipe - the ids in it would resolve to
			// nothing. Treat it like a delta and ask for the dictionary.
			if (!AcceptGeneration(packet.generation, false))
				return;

			const int count = (std::min)(static_cast<int>(packet.count), kBaseStatsPerPacket);
			for (int i = 0; i < count; ++i)
			{
				const PvBaseStatsPacket::Entry& entry = packet.entries[i];

				auto itemIt = g_remoteItemKeys.find(entry.itemId);
				if (itemIt == g_remoteItemKeys.end())
					continue;

				auto baseIt = g_remoteBases.find(entry.baseId);
				if (baseIt == g_remoteBases.end())
					continue;

				ProductionTracker::SeedRemoteBase(itemIt->second, baseIt->second.key,
					baseIt->second.name, entry.produced, entry.consumed);
			}

			MarkPacketReceived();
		}

		void OnResetPacket(const PvResetPacket& packet)
		{
			if (packet.schemaVersion != kSchemaVersion)
				return;

			LOG_INFO("ProductionNet: server reset the production feed (generation %u)", packet.generation);
			WipeRemoteState();
			g_remoteGeneration = packet.generation;
			g_haveServerData.store(false, std::memory_order_relaxed);
			SetWaitReason(WaitReason::NoServerData);
		}

		void OnHelloPacket(void* senderController, const PvHelloPacket& packet)
		{
			if (packet.schemaVersion != kSchemaVersion)
				return;
			SendSnapshotTo(senderController);
		}

		// Authority: this client has now reported our plugin at our version, so
		// this is the first moment a packet to it can actually land. It is
		// strictly later than the player-joined hook - see Init.
		void OnClientReady(void* playerController)
		{
			SendSnapshotTo(playerController);
		}

		// Client: the authority has acknowledged our manifest. Ask for a
		// snapshot right now rather than waiting out the retry cooldown.
		void OnServerReady(const char* serverBuildTag)
		{
			LOG_INFO("ProductionNet: server link ready (loader build %s)",
				serverBuildTag ? serverBuildTag : "unknown");
			g_serverReadySignal.store(true, std::memory_order_relaxed);
		}

		void SendHello()
		{
			IPluginSelf* self = GetSelf();
			IPluginHooks* hooks = GetHooks();
			if (!self || !hooks || !hooks->Network)
				return;

			// The net mode resolves to Client well before the authority has
			// acknowledged our manifest, and anything sent in between is dropped
			// (and warned about). Leave the request pending with its cooldown
			// untouched so it goes out on the frame the link opens, instead of
			// burning a retry interval on a packet nobody will see.
			if (!hooks->Network->IsServerReady())
				return;

			PvHelloPacket hello{};
			hello.schemaVersion = kSchemaVersion;
			Network::SendPacketToServer(hooks, self, hello);

			g_helloPending  = false;
			g_helloCooldown = kHelloRetrySeconds;
		}

		// ---- Per-role ticks -------------------------------------------------

		// Authority is a runtime property, not a build-time one: a listen host is
		// a client build that holds it. The loader's client-ready callback checks
		// for it at registration time and warns if it is absent, and it is always
		// absent at PluginInit - there is no world yet to ask - so registration
		// waits for the role to resolve. Nothing is missed by being late: the
		// callback fires immediately for every client already ready.
		void EnsureClientReadyRegistered()
		{
			if (g_clientReadyRegistered)
				return;

			IPluginSelf* self = GetSelf();
			IPluginHooks* hooks = GetHooks();
			if (!self || !hooks || !hooks->Network)
				return;

			hooks->Network->RegisterClientReadyCallback(self, &OnClientReady);
			g_clientReadyRegistered = true;
			LOG_INFO("ProductionNet: serving production data to clients");
		}

		void TickAuthority(float deltaSeconds)
		{
			g_broadcastAccum += deltaSeconds;
			g_sinceLastSend  += deltaSeconds;
			g_fullSyncAccum  += deltaSeconds;

			// A snapshot request is answered on the frame it is noticed, and
			// only after the pending deltas have gone out (see
			// ServiceSnapshotRequests).
			bool snapshotDue = false;
			{
				std::lock_guard<std::mutex> lock(g_snapshotMutex);
				snapshotDue = !g_snapshotRequests.empty();
			}

			const bool fullSyncDue = g_fullSyncAccum >= g_fullSyncInterval;

			if (!snapshotDue && !fullSyncDue && g_broadcastAccum < g_broadcastInterval)
				return;

			g_broadcastAccum = 0.0f;
			FlushAuthority();

			if (!fullSyncDue && !snapshotDue)
				return;

			// Built once and used by both paths - and only after FlushAuthority,
			// so every absolute figure here is newer than every delta ahead of it
			// on the wire. A craft landing in the gap between the pending buffer
			// being swapped above and the tracker being read here is counted in
			// both, which the next full sync then corrects; it is worth one
			// interval of one item rather than a lock held across both modules.
			FullState state;
			BuildFullState(state, fullSyncDue);

			if (fullSyncDue)
			{
				g_fullSyncAccum = 0.0f;
				BroadcastFullSync(state);
			}

			ServiceSnapshotRequests(state);
		}

		void TickRemoteClient(float deltaSeconds)
		{
			if (g_helloCooldown > 0.0f)
				g_helloCooldown -= deltaSeconds;

			if (g_serverReadySignal.exchange(false, std::memory_order_relaxed))
			{
				g_helloPending  = true;
				g_helloCooldown = 0.0f;
			}

			if (g_helloPending && g_helloCooldown <= 0.0f)
				SendHello();

			if (!g_haveServerData.load(std::memory_order_relaxed))
			{
				SetWaitReason(WaitReason::NoServerData);

				// Either the server has no plugin, or it loaded one after we
				// joined. Keep asking, slowly.
				if (g_helloCooldown <= 0.0f)
					g_helloPending = true;
				return;
			}

			const float age = static_cast<float>(NowSeconds() - g_lastPacketTime.load(std::memory_order_relaxed));
			if (age > g_staleAfterSeconds)
			{
				SetWaitReason(WaitReason::LinkStale);
				if (g_helloCooldown <= 0.0f)
					g_helloPending = true;
			}
			else
			{
				SetWaitReason(WaitReason::None);
			}
		}
	}

	const char* RoleName(SessionRole role)
	{
		switch (role)
		{
			case SessionRole::Solo:            return "Solo";
			case SessionRole::ListenHost:      return "Listen host";
			case SessionRole::DedicatedServer: return "Dedicated server";
			case SessionRole::RemoteClient:    return "Client";
			default:                           return "Unresolved";
		}
	}

	const char* WaitReasonText(WaitReason reason)
	{
		switch (reason)
		{
			case WaitReason::DetectingSession: return "Detecting session...";
			case WaitReason::NoServerData:     return "Waiting for production data from the server...";
			case WaitReason::LinkStale:        return "No update from the server recently.";
			default:                           return "";
		}
	}

	void Init(IPluginSelf* self)
	{
		IPluginHooks* hooks = self ? self->hooks : nullptr;
		if (!hooks || !hooks->Network)
		{
			LOG_WARN("ProductionNet: network channel unavailable - production data will not be synchronised");
			return;
		}

		// Answering a hello is an authority job, but which process holds
		// authority is not known until a world exists - and a listen host is a
		// client build that holds it. Both builds therefore register, and the
		// loader gates the reply on the net mode at send time.
		g_helloHandler = Network::OnServerReceive<PvHelloPacket>(hooks, self, &OnHelloPacket);

		// The client-ready callback is the one thing that cannot be registered
		// here - it is refused when this process is not yet the authority. See
		// EnsureClientReadyRegistered, which the tick calls once the role is
		// known. It is deliberately NOT the player-joined hook: a client is not
		// ready when PostLogin fires, so a snapshot sent there is dropped.

#if !defined(MODLOADER_SERVER_BUILD)
		g_itemDefHandler   = Network::OnReceive<PvItemDefPacket>(hooks, self, &OnItemDefPacket);
		g_baseDefHandler   = Network::OnReceive<PvBaseDefPacket>(hooks, self, &OnBaseDefPacket);
		g_deltaHandler     = Network::OnReceive<PvDeltaPacket>(hooks, self, &OnDeltaPacket);
		g_baseStatsHandler = Network::OnReceive<PvBaseStatsPacket>(hooks, self, &OnBaseStatsPacket);
		g_resetHandler     = Network::OnReceive<PvResetPacket>(hooks, self, &OnResetPacket);

		// The mirror of the hello handler, for our own outgoing hello. Never
		// fires on a listen host: there is no server for it to be ready for.
		hooks->Network->RegisterServerReadyCallback(self, &OnServerReady);
#endif
	}

	void Shutdown(IPluginSelf* self)
	{
		IPluginHooks* hooks = self ? self->hooks : nullptr;
		if (hooks && hooks->Network)
		{
			if (g_itemDefHandler)
				hooks->Network->UnregisterMessageHandler(self, typeid(PvItemDefPacket).name(), g_itemDefHandler);
			if (g_baseDefHandler)
				hooks->Network->UnregisterMessageHandler(self, typeid(PvBaseDefPacket).name(), g_baseDefHandler);
			if (g_deltaHandler)
				hooks->Network->UnregisterMessageHandler(self, typeid(PvDeltaPacket).name(), g_deltaHandler);
			if (g_baseStatsHandler)
				hooks->Network->UnregisterMessageHandler(self, typeid(PvBaseStatsPacket).name(), g_baseStatsHandler);
			if (g_resetHandler)
				hooks->Network->UnregisterMessageHandler(self, typeid(PvResetPacket).name(), g_resetHandler);
			if (g_helloHandler)
				hooks->Network->UnregisterServerMessageHandler(self, typeid(PvHelloPacket).name(), g_helloHandler);

			if (g_clientReadyRegistered)
				hooks->Network->UnregisterClientReadyCallback(self, &OnClientReady);

#if !defined(MODLOADER_SERVER_BUILD)
			hooks->Network->UnregisterServerReadyCallback(self, &OnServerReady);
#endif
		}

		g_itemDefHandler   = nullptr;
		g_baseDefHandler   = nullptr;
		g_deltaHandler     = nullptr;
		g_baseStatsHandler = nullptr;
		g_resetHandler     = nullptr;
		g_helloHandler     = nullptr;

		g_clientReadyRegistered = false;
	}

	void OnSessionLoaded()
	{
		{
			std::lock_guard<std::mutex> lock(g_authorityMutex);
			g_itemDict.clear();
			g_baseDict.clear();
			g_pending.clear();
			g_nextItemId = 0;
			g_nextBaseId = 0;
			++g_generation;
			g_sequence = 0;
		}

		g_broadcastAccum = 0.0f;
		g_sinceLastSend  = 0.0f;
		g_fullSyncAccum  = 0.0f;

		g_remoteItemKeys.clear();
		g_remoteBases.clear();
		g_lastSequence  = 0;
		g_haveSequence  = false;
		g_helloPending  = true;
		g_helloCooldown = 0.0f;
		g_haveServerData.store(false, std::memory_order_relaxed);

		// The world this data described is gone; tell clients before they draw
		// another frame of it.
		if (RoleHasAuthority(g_role.load(std::memory_order_relaxed)))
		{
			PvResetPacket reset{};
			reset.schemaVersion = kSchemaVersion;
			reset.generation    = g_generation;
			Broadcast(reset);
		}
	}

	void Tick(float deltaSeconds)
	{
		IPluginHooks* hooks = GetHooks();
		if (!hooks)
			return;

		RefreshTunables(deltaSeconds);

		// Net mode only resolves once an actor exists, so keep asking rather
		// than assuming a role and tracking (or hiding) the wrong thing.
		SessionRole role = g_role.load(std::memory_order_relaxed);
		if (role == SessionRole::Unresolved && hooks->NetMode)
		{
			const SessionRole resolved = RoleFromNetMode(hooks->NetMode->GetNetMode());
			if (resolved != SessionRole::Unresolved)
			{
				role = resolved;
				g_role.store(role, std::memory_order_relaxed);
				LOG_INFO("ProductionNet: session role resolved: %s", RoleName(role));

				// The crafting hook can't know it's on a client until this
				// moment, so a handful of local (and therefore partial)
				// samples may already have landed. Throw them away rather than
				// letting them sit under the server's figures.
				if (role == SessionRole::RemoteClient)
					ProductionTracker::Clear();
			}
		}

		if (role == SessionRole::Unresolved)
		{
			SetWaitReason(WaitReason::DetectingSession);
			return;
		}

		if (role == SessionRole::Solo)
		{
			SetWaitReason(WaitReason::None);
			return;
		}

		// Solo returned above, so this is a listen host or a dedicated server:
		// an authority that actually has clients to serve.
		if (RoleHasAuthority(role))
		{
			SetWaitReason(WaitReason::None);
			EnsureClientReadyRegistered();
			TickAuthority(deltaSeconds);
			return;
		}

		TickRemoteClient(deltaSeconds);
	}

	void OnLocalSample(const std::string& itemKey, const std::string& displayName,
		float amount, bool isProduction, const std::vector<BaseSample>& bases)
	{
		if (amount <= 0.0f)
			return;

		// Solo has nobody to tell; queueing there would just grow the pending
		// buffer forever. Samples taken before the role resolves are dropped
		// here, which the full sync then puts back - it reads the tracker, which
		// has been recording them all along.
		const SessionRole role = g_role.load(std::memory_order_relaxed);
		if (role != SessionRole::ListenHost && role != SessionRole::DedicatedServer)
			return;

		std::lock_guard<std::mutex> lock(g_authorityMutex);

		// Ids run from 0 upward, so "is this new?" is an insertion result, not
		// a test on the id itself. kBaseIdItemTotal is reserved, which also
		// caps how many distinct items and bases a session can replicate -
		// orders of magnitude above any real base, but a wrap would silently
		// alias two of them, so stop instead.
		auto [itemIt, itemIsNew] = g_itemDict.try_emplace(itemKey);
		ItemDictEntry& itemDict = itemIt->second;
		if (itemIsNew)
		{
			if (g_nextItemId >= kBaseIdItemTotal)
			{
				g_itemDict.erase(itemIt);
				LOG_WARN("ProductionNet: item id space exhausted (%u) - '%s' will not be replicated",
					static_cast<unsigned>(g_nextItemId), itemKey.c_str());
				return;
			}
			itemDict.id = g_nextItemId++;
			itemDict.displayName = displayName;
			itemDict.defSent = false;
		}

		PendingItem& pending = g_pending[itemKey];
		pending.id = itemDict.id;
		if (isProduction)
			pending.produced += amount;
		else
			pending.consumed += amount;

		for (const BaseSample& base : bases)
		{
			auto [baseIt, baseIsNew] = g_baseDict.try_emplace(base.key);
			BaseDictEntry& baseDict = baseIt->second;
			if (baseIsNew)
			{
				if (g_nextBaseId >= kBaseIdItemTotal)
				{
					g_baseDict.erase(baseIt);
					continue;
				}
				baseDict.id = g_nextBaseId++;
			}

			// Covers first sighting and later renames alike - the panel shows
			// base names, so a rename has to reach clients.
			if (baseDict.name != base.name)
			{
				baseDict.name = base.name;
				baseDict.defDirty = true;
			}

			// Base cores don't move, so a location is sent once - but it can be
			// unknown on the first craft and resolve later.
			if (base.hasLocation && !baseDict.hasLocation)
			{
				baseDict.hasLocation = true;
				baseDict.locX = base.locX;
				baseDict.locY = base.locY;
				baseDict.locZ = base.locZ;
				baseDict.defDirty = true;
			}

			PendingBase& pendingBase = pending.bases[base.key];
			pendingBase.id = baseDict.id;
			if (isProduction)
				pendingBase.produced += amount;
			else
				pendingBase.consumed += amount;
		}
	}

	void SendSnapshotTo(void* playerController)
	{
		if (!playerController)
			return;

		std::lock_guard<std::mutex> lock(g_snapshotMutex);
		for (void* existing : g_snapshotRequests)
			if (existing == playerController)
				return;
		g_snapshotRequests.push_back(playerController);
	}

	SessionRole GetRole()
	{
		return g_role.load(std::memory_order_relaxed);
	}

	bool IsRemoteClient()
	{
		return g_role.load(std::memory_order_relaxed) == SessionRole::RemoteClient;
	}

	WaitReason GetWaitReason()
	{
		return static_cast<WaitReason>(g_waitReason.load(std::memory_order_relaxed));
	}

	float GetDataAgeSeconds()
	{
		const double last = g_lastPacketTime.load(std::memory_order_relaxed);
		if (last <= 0.0)
			return -1.0f;
		return static_cast<float>(NowSeconds() - last);
	}
}

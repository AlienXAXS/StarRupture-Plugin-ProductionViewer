#pragma once

#include <cstdint>

// Wire format for the server -> client production feed.
//
// The Client and Server DLL builds must agree on these layouts byte for byte:
// the ModLoader routes packets on plugin name + typeid(T).name(), so a
// mismatch here silently drops packets rather than failing loudly. Bump
// kSchemaVersion whenever a field changes size, order or meaning - the
// receiving side rejects anything it doesn't recognise.
//
// Everything is fixed-size POD: Network::SendPacket* sends sizeof(T), so
// variable-length data is expressed as a fixed array plus a count, and long
// runs are split across several packets.
namespace ProductionNet
{
	constexpr uint8_t kSchemaVersion = 2;

	// Fixed string capacities, all including the null terminator. Item keys are
	// UAuItemDataBase::UniqueItemName (short internal identifiers); base names
	// are player-assigned and are truncated rather than split across packets.
	constexpr int kItemKeyChars  = 48;
	constexpr int kItemNameChars = 48;
	constexpr int kBaseNameChars = 48;

	// Entries per packet, sized so every payload stays well under the
	// ModLoader's 1400-byte limit (packets travel base64-encoded inside a
	// gameplay RPC, so the encoded form is a third larger again).
	constexpr int kItemDefsPerPacket  = 4;   // 456-byte payload
	constexpr int kBaseDefsPerPacket  = 6;   // 440-byte payload
	constexpr int kDeltasPerPacket    = 64;  // 780-byte payload
	constexpr int kBaseStatsPerPacket = 64;  // 776-byte payload

	// A delta addressed to the item's own totals rather than to one of its base
	// cores. Item totals are not the sum of the per-base figures - a crafter
	// standing inside two overlapping base core areas counts once toward the
	// item and once toward each base - so both are carried explicitly.
	constexpr uint16_t kBaseIdItemTotal = 0xFFFF;

#pragma pack(push, 1)

	// Item dictionary. Interns an item key and display name under a small id so
	// the high-frequency delta packets can carry ids instead of strings, and
	// seeds the client's All Time totals from the server's so a joining player
	// sees the base's real history instead of starting from zero.
	//
	// The totals are absolute, and the client applies them by overwrite, so
	// re-sending this packet costs nothing but bandwidth and repairs any amount
	// of accumulated drift. That is what the periodic full sync relies on.
	struct PvItemDefPacket
	{
		uint8_t  schemaVersion;
		uint8_t  count;          // entries in use
		uint16_t reserved;
		uint32_t generation;

		struct Entry
		{
			uint16_t itemId;
			uint16_t reserved;
			float    allTimeProduced;
			float    allTimeConsumed;
			float    allTimeElapsed;             // seconds the item has been tracked
			char     key[kItemKeyChars];         // UniqueItemName - also the client's icon lookup key
			char     displayName[kItemNameChars];
		} entries[kItemDefsPerPacket];
	};

	// Base core dictionary. The world location travels with it so a client can
	// show distances and drive the "Go" waypoint without resolving the base core
	// entity itself - Mass entity handles are local to one process and mean
	// nothing on the other end of the wire.
	struct PvBaseDefPacket
	{
		uint8_t  schemaVersion;
		uint8_t  count;
		uint16_t reserved;
		uint32_t generation;

		struct Entry
		{
			uint16_t baseId;
			uint8_t  hasLocation;
			uint8_t  reserved;
			uint64_t baseKey;      // ProductionBaseCore::PackHandle; 0 = "Unknown Location"
			float    locX;
			float    locY;
			float    locZ;
			char     name[kBaseNameChars];
		} entries[kBaseDefsPerPacket];
	};

	// Production and consumption since the previous delta packet. An item or
	// base absent from a packet simply had no activity in that interval: the
	// client keeps the totals it already holds, and its own rolling windows
	// decay on their own. That is the whole compression scheme - a base running
	// unchanged costs nothing beyond the packet header.
	//
	// Deltas are the only thing on the wire that is NOT self-correcting: one lost
	// packet is one interval of production the client will never hear about
	// again. The periodic full sync (absolute totals) is what bounds that error,
	// and the sequence number below is what makes the loss visible in the log.
	struct PvDeltaPacket
	{
		uint8_t  schemaVersion;
		uint8_t  count;
		uint16_t reserved;
		uint32_t generation;
		uint32_t sequence;       // monotonic; a gap tells the client to resync

		struct Entry
		{
			uint16_t itemId;
			uint16_t baseId;     // kBaseIdItemTotal for the item's own totals
			float    produced;
			float    consumed;
		} entries[kDeltasPerPacket];
	};

	// Absolute per-base-core totals, the counterpart of PvItemDefPacket for the
	// breakdown rows. Deltas alone can only ever describe the time since a client
	// connected, and unlike the item totals there is nowhere else for the real
	// figures to ride along - so they travel here, and are applied by overwrite.
	//
	// Only the All Time totals are carried. The client's rolling 1m/10m/1h tiers
	// stay delta-driven and so cover the time since it synced, exactly as they do
	// for the item-level figures.
	struct PvBaseStatsPacket
	{
		uint8_t  schemaVersion;
		uint8_t  count;
		uint16_t reserved;
		uint32_t generation;

		struct Entry
		{
			uint16_t itemId;
			uint16_t baseId;     // never kBaseIdItemTotal - item totals ride in the def
			float    produced;   // all time, not an interval
			float    consumed;
		} entries[kBaseStatsPerPacket];
	};

	// Broadcast when the server switches save. Everything the client holds
	// belongs to a world it is no longer in, so it drops the lot and re-syncs.
	struct PvResetPacket
	{
		uint8_t  schemaVersion;
		uint8_t  reserved[3];
		uint32_t generation;
	};

	// Client -> server: "send me the dictionary and the current totals." Sent on
	// join, on a generation change, and whenever a sequence gap says a delta
	// went missing.
	struct PvHelloPacket
	{
		uint8_t schemaVersion;
		uint8_t reserved[3];
	};

#pragma pack(pop)

	static_assert(sizeof(PvItemDefPacket)   == 456, "PvItemDefPacket layout changed");
	static_assert(sizeof(PvBaseDefPacket)   == 440, "PvBaseDefPacket layout changed");
	static_assert(sizeof(PvDeltaPacket)     == 780, "PvDeltaPacket layout changed");
	static_assert(sizeof(PvBaseStatsPacket) == 776, "PvBaseStatsPacket layout changed");
	static_assert(sizeof(PvResetPacket)     == 8,   "PvResetPacket layout changed");
	static_assert(sizeof(PvHelloPacket)     == 4,   "PvHelloPacket layout changed");
}

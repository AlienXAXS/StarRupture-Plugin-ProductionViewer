#pragma once

#include "plugin_interface.h"

#include <cstdint>
#include <string>
#include <vector>

// Multiplayer replication for the production feed.
//
// Only the server sees every crafter: a client's Mass simulation covers what
// the engine has streamed in around that player, so tracking locally on a
// client produces numbers that quietly disagree with the base's real output.
// The server is therefore the sole authority - it tracks, and it pushes the
// results to everyone else, who display them and track nothing themselves.
//
// The feed is incremental. New items and base cores are announced once in a
// dictionary packet that interns them under a small id; after that only the
// amounts crafted since the last broadcast go over the wire, and anything that
// produced nothing in an interval is simply left out (see production_packets.h).
//
// On top of that sits a periodic full sync: every FullSyncInterval seconds the
// authority re-states every figure it holds, in absolute terms, and the client
// takes those over whatever it had. Deltas are the only part of the feed that
// cannot heal - one lost packet is one interval of production a client would
// otherwise never hear about again - and the incremental path alone can only
// ever describe items something has crafted since the process started, never
// the ones a restored save is still carrying. The full sync answers both, and
// is why the two ends converge instead of drifting apart.
//
// Note on listen hosts: a listen host is a client build that holds net
// authority, which is a runtime property. It broadcasts to its clients like any
// other authority - see EnsureClientReadyRegistered for the one piece of that
// which cannot be set up at load time.
namespace ProductionNet
{
	// Where this plugin instance sits in the session. Resolved from
	// EPluginNetMode, which reports Unknown until an actor exists to query -
	// hence the explicit Unresolved value rather than assuming a role and
	// showing the wrong thing for the first few frames.
	enum class SessionRole : uint8_t
	{
		Unresolved      = 0,
		Solo            = 1,   // Standalone - local authority, nobody to tell
		ListenHost      = 2,   // ListenServer - authority + UI
		DedicatedServer = 3,   // authority, no UI
		RemoteClient    = 4,   // display only
	};

	inline bool RoleHasAuthority(SessionRole role)
	{
		return role == SessionRole::Solo
		    || role == SessionRole::ListenHost
		    || role == SessionRole::DedicatedServer;
	}

	const char* RoleName(SessionRole role);

	// Why a remote client has nothing to draw. Surfaced verbatim in the panel so
	// "still connecting" reads differently from "this server has no plugin".
	enum class WaitReason : uint8_t
	{
		None = 0,
		DetectingSession,   // net mode still Unknown
		NoServerData,       // never received a packet
		LinkStale,          // last packet older than the configured timeout
	};

	const char* WaitReasonText(WaitReason reason);

	// Registers the packet handlers this build can use. Safe to call when
	// hooks->Network is null (generic builds); everything then no-ops.
	void Init(IPluginSelf* self);
	void Shutdown(IPluginSelf* self);

	// Drops per-session state and, on the server, tells clients to do the same.
	// Call when a save (re)loads.
	void OnSessionLoaded();

	// Game-thread tick, driven from ProductionTracker's engine tick. Resolves
	// the session role, broadcasts on the configured interval, and ages out
	// client-side data.
	void Tick(float deltaSeconds);

	// One base core a craft was attributed to. Mirrors
	// ProductionBaseCore::BaseCoreInfo, flattened to the packed handle key and
	// plain floats the wire format uses — this module deliberately pulls in no
	// SDK headers, so it compiles identically against the client and server
	// SDKs.
	struct BaseSample
	{
		uint64_t    key = 0;   // ProductionBaseCore::PackHandle; 0 = "Unknown Location"
		std::string name;

		// The base core's world location, so clients can show distances and
		// drive the waypoint without resolving the entity themselves.
		bool  hasLocation = false;
		float locX = 0.0f;
		float locY = 0.0f;
		float locZ = 0.0f;
	};

	// Authority: queue one craft completion for the next broadcast. Called from
	// ProductionTracker::RecordSample, and a cheap no-op when there is nobody to
	// send to (solo, or a build that cannot send).
	void OnLocalSample(const std::string& itemKey, const std::string& displayName,
		float amount, bool isProduction, const std::vector<BaseSample>& bases);

	// Server: push the full dictionary and current totals to one player, so a
	// joining client doesn't sit on an empty panel until something gets crafted.
	// Queued and sent on the next authority tick. Driven by the loader's
	// client-ready callback, never by player-joined: a client is not reachable
	// yet at PostLogin, and packets sent then are dropped, not buffered.
	void SendSnapshotTo(void* playerController);

	SessionRole GetRole();

	// True once the role is known to be a remote client: this instance must not
	// track, persist or display anything of its own.
	bool IsRemoteClient();

	// Client display status. Both are meaningless on an authority.
	WaitReason GetWaitReason();
	float      GetDataAgeSeconds();
}

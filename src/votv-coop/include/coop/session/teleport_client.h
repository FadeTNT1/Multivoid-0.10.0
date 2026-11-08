// coop/session/teleport_client.h -- teleport connected clients to the host's pose.
//
// A shipped moderation verb, not a dev toy: /tphere (the scoreboard's Teleport runs it) brings one
// client to the host through TeleportSlotToHostWithToken (moderation.cpp), event_feed applies the wire packet on the receiving
// side, and the F1 dev-menu button broadcasts to everyone. A JOIN does not use the wire verb -- a
// joiner places itself (net_pump, through ApplyLocally below), and a host teleport at world-ready
// overwrote that placement, since world-ready arrives after the client has already spawned.
//
// Direction: HOST -> CLIENT only. The action self-gates on Session::Role::Host and no-ops on a
// client. It mirrors MTA's `!tphere` chat command: /tphere is the command, and the scoreboard's
// Teleport button submits that line.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::teleport_client {

// Cache the Session pointer so the action can snapshot host pose + broadcast.
// Called once from harness boot.
void SetSession(coop::net::Session* session);

// Menu action (Player > Movement): HOST snapshots its own mainPlayer Location +
// Rotation and sends it to the clients (they K2_TeleportTo). HOST-only -- on a
// client this logs + no-ops. Safe to call off the game thread (the snapshot is
// posted to it).
void TeleportClientsToHost();

// The /tphere command: teleport ONE specific client (the peer at `peerSlot`,
// 1..kMaxPeers-1) to the host's pose, only while `generation` is still that slot's
// occupancy generation, so a successor admitted into the seat is never moved. GAME
// THREAD. The pose is read now and sent by a queued task that checks the generation
// again. Returns false, sending nothing, when this is not the host, the slot is out
// of range, the generation is not the slot's, or the host's pose cannot be read; the
// caller tells a changed seat from the rest by reading the slot's generation again.
bool TeleportSlotToHostWithToken(int peerSlot, uint32_t generation);

// What /tpto came to (TeleportToPlayer below).
enum class ToPlayer : uint8_t {
    Done,           // the caller was moved (or told to move) beside the target
    NoSession,      // not a running hosted session
    SameBody,       // the caller named themselves
    CallerGone,     // the caller's seat changed hands
    TargetGone,     // the target's seat changed hands
    CallerAway,     // the caller is in a pocket dimension or still loading
    TargetAway,     // the target is in a pocket dimension
    TargetJoining,  // the target's world is not up yet
    TargetDead,     // the target is dead
    NoSpot,         // no clear standing place beside the target
    Failed,         // a pose could not be read or the move could not be sent
};

// HOST, game thread. Move the player at `callerSlot` (0 is the host) beside the player at `targetSlot`, each
// seat checked against its occupancy generation (0 for the host). The destination is chosen here, on the host:
// the first of a ring of places around the target, behind first, with a clear line from the target and a floor
// and head room (coop/player/anti_noclip_rescue StandingAt). The host moves itself through ApplyLocally and a
// client through the TeleportClient lane, whose verb rewrites mainPlayer's `lastLoc`, so the game's anti-noclip
// check sees an arrival, not a crossing of walls. Refuses a target in another level, still joining or dead.
ToPlayer TeleportToPlayer(coop::net::Session& session, int callerSlot, uint32_t callerGen, int targetSlot,
                          uint32_t targetGen);

// Receiver: apply the teleport on the local mainPlayer (K2_TeleportTo).
// Called from event_feed.cpp on incoming ReliableKind::TeleportClient AND
// gated to client-role receivers (host echo is a no-op). Game thread only.
struct ApplyArgs {
    float locX, locY, locZ;
    float rotPitch, rotYaw, rotRoll;
};
// Returns whether a teleport call REPORTED success (the primary teleportWObackrooms, or
// one of the two fallbacks). It is a report about the CALL, not about where the player
// ended up: the three-tier fallback means "something was dispatched", and VOTV constraints
// can still revert a K2_TeleportTo. A caller that needs to know the player actually MOVED
// must read the position back (coop::death_revive's revive conjunction does exactly that).
bool ApplyLocally(const ApplyArgs& args);

}  // namespace coop::teleport_client

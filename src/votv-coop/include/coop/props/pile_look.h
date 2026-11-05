// coop/props/pile_look.h -- the host's look of a chip pile, on every peer.
//
// The mesh a player sees on a pile is a child of the pile's root, and the game's init() turns and
// scales it at random on every construction, a save load included (ue_wrap/actors/chip_pile.h), so
// two peers that load one save draw two looks for one pile. The host's draw is the authority: it
// rides every expression of a pile (PropSpawn, a kToPile PropConvert) as a WirePileLook.
//
// A client binds an actor to a host eid at many moments (the join bracket's own-save bind, a
// materialised mirror, the quiescence re-bind after GC churn) and the look can arrive before or
// after any of them, so it is kept per eid: whichever of the two arrives second completes the
// pair, at remote_prop::RegisterPropMirror, the one entry every bind goes through. Mid-join needs
// nothing of its own: the snapshot expresses every pile, and each expression carries the look.
// MTA keeps such state ON the element (an object's scale: CEntityAddPacket, CObjectRPCs); the side
// map per eid is the divergence, because a look can arrive before its element exists. Game thread.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::pile_look {

// The look of `actor` for the wire; absent (sclX 0) when `actor` is not a pile. Any peer.
coop::net::WirePileLook Capture(void* actor);

// An expression of `eid` arrived from `senderSlot` carrying `look`. Only the host's (slot 0) is
// taken: the host runs the same receive path for a client's PropSpawn, and a client's word must
// not re-skin the authority's pile. Kept, and applied now when `eid` already has a live pile
// bound. An absent look is ignored.
void OnHostLook(uint32_t eid, const coop::net::WirePileLook& look, int senderSlot);

// The root scale the host's expression of `eid` carried, in the form it named (`isClump`), kept
// beside the look and taken by the same rule (the host's word only, an all-zero scale ignored). An
// actor of that form bound to `eid` later -- a re-bind after GC churn, which carries no row of its
// own -- takes it at the bind, so a pile keeps the host's size across every bind and re-bind.
void OnHostRootScale(uint32_t eid, float sx, float sy, float sz, bool isClump, int senderSlot);

// `actor` was just bound to `eid`. Applies the kept look, if one arrived first, and the kept root
// scale when the actor is of the form it was kept for. A no-op for an actor that is not trash, and
// on the host, where nothing is ever kept.
void OnBound(uint32_t eid, void* actor);

// One line with the look READ BACK off `actor`, at a land: the host's says what it sent, a
// client's what its pile shows after the apply, and a drill compares the two. Rare (one per land).
void LogLandLook(const char* who, uint32_t eid, void* actor);

// `eid` is gone.
void Forget(uint32_t eid);

// The session ended: every kept look goes with it.
void OnDisconnect();

}  // namespace coop::pile_look

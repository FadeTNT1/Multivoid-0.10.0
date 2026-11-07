// coop/interactables/atv_repair.h -- the ATV's repairs in co-op: a battery, a spare or a wheel fitted from the hand,
// a wheel or the battery unscrewed, a gas can's refuel and a toolbox fix, made by any peer and kept by the host.
//
// What runs where. An item in a player's hand is that player's own (its inventory), so a CLIENT's fit, refuel and fix
// run the game's own body on the client's copy and the values that body left are sent to the host (AtvRepairIntent):
// a POST on playerUsedOn, putTire, fuelUp and toolboxFix compares the copy's condition with the PRE's and names what
// it gained. An unscrew births a prop (the wheel, or the battery with its charge), which only the host may author, so
// a client's unscrewPanel is cancelled, its unscrew ended (resetUnscrew) and the host runs the game's own
// unscrewPanel for the same wheel or for the battery. The host checks each request against its own copy (the sender in
// reach; no battery in, no spare, the socket empty, the fuel lower, the health below full, the wheel or the battery
// there to take off) and writes or runs it. After any repair on the host's copy, a client's or the host's own, the
// host sends every client a block of its copy marked kAtvRepair (atv_sync.cpp), which a client applies, presence
// included, even to an ATV it syncs: the condition lane takes presence only from the host's own copy, so without it a
// fit on the host never reached a client that drove the ATV, and a client's fit never reached anyone.
//
// Gameplay layer (principle 7); the engine half is ue_wrap/devices/atv_repair and ue_wrap/devices/atv_condition.
// Game thread, everything.

#pragma once

#include <cstdint>

namespace coop::net { class Session; struct AtvRepairIntentPayload; }

namespace coop::atv_repair {

// Registers the five watches, retried once a second until each is in; the per-tick ensure path, called from
// atv_sync::Install. Remembers the session.
void Install(coop::net::Session* session);

// HOST: a client's AtvRepairIntent from `slot` (1..kMaxPeers-1, checked by the dispatch).
void OnIntent(coop::net::Session& session, const coop::net::AtvRepairIntentPayload& p, uint8_t slot);

// HOST: whether a repair changed `actor`'s copy since the last ask; true once per change. atv_sync's Tick sends the
// kAtvRepair block on a yes.
bool TakeDue(void* actor);

// The session's state cleared at a disconnect (the due set, the paces, the PRE snapshots), with a summary line.
void Reset();

}  // namespace coop::atv_repair

// coop/interactables/physmods_sync.h -- the desk PHYSICAL-MODULES array sync (PhysModsState).
//
// Slot ops over a host-canonical array of 12 slots. The desk takes two modules of one type
// (isModuleAllowed is a list of types, not a duplicate check), so an op names its slot as well as its
// module. DETECT is at the verbs: the desk's two bodies that edit the array from play, plugInModule and
// actionOptionIndex (the E press, whose unplug empties a slot), are watched at the script gate, and each
// sends what its body changed; the save load's setData is not an edit, so a joiner's own load sends
// nothing. The OPS are plug{slot, byte} and unplug{slot, byte}, peer to host and host-terminal: the HOST
// applies to ITS array, runs updPhysMods, then broadcasts the FULL canonical array, and every peer -- the
// presser included -- adopts wholesale and re-runs updPhysMods, idempotent as a pure function of the
// array. A DENY goes back to the author of an op that lost a race, with the canonical array after it: a
// PLUG into a slot taken first is refunded by a host respawn at the desk, and an UNPLUG of a slot emptied
// first destroys the author's hand ghost, or sweeps its untracked actors of that byte when the module was
// already dropped. The module props ride the destroy seam and the birth watchers. JOIN: the save transfer
// seeds the array; the host ships the canonical array in connect-replay, which parks a pre-desk joiner.
//
// BACK PANELS. The desk's four removable back panels (panelsState, true = on) cover the module slots; the E press
// on an unscrewed one clears its flag and spawns the loose panel prop, a panel prop overlapping the desk sets the
// flag and destroys the prop. The same watchers snapshot panelsState, and an edit is op 4 (panel set, peer to
// host); the host writes it, runs updPanels and broadcasts op 5, the four flags, which every peer adopts and
// re-runs updPanels on, idempotent. The loose prop rides the spawn and destroy lanes, so a client's take-off
// spawns one prop (the host never respawns it). The desk's overlap entry refuses a panel a remote player carries.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::physmods_sync {

void Install(coop::net::Session* session);

// Per-net-pump: applies a canonical that arrived before the desk resolved, once it has.
void Tick();

// PhysModsState from the wire (router: event_dispatch_signal.cpp).
void OnPhysMods(const coop::net::PhysModsStatePayload& p, uint8_t senderSlot);

// HOST: ship the canonical array to a joiner (connect replay).
void QueueConnectBroadcastForSlot(int slot);

// How many canonical arrays this peer has written into its desk: the host applies an op and then
// broadcasts, so a client counting past its own ops knows the host applied them. Game thread.
uint64_t CanonicalsAdopted();

// HOST consult from the ReelEjectIntent birth author: reap a module birth that matches a
// fresh unplug-deny for this sender, which makes it the ghost of a drop that raced the
// deny. True = refuse.
bool HostShouldReapModuleBirth(uint8_t senderSlot, void* moduleClass);

// Full state reset. Wired into the subsystems teardown fanout.
void OnDisconnect();

}  // namespace coop::physmods_sync

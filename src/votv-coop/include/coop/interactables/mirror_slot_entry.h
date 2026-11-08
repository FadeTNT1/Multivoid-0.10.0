// coop/interactables/mirror_slot_entry.h -- a device never takes a prop another player is carrying.
//
// A device takes a prop by its own overlap: a drive entering a receiver port, a drive box or the rack, a module
// entering a desk slot, a back panel entering the desk's frame, a lid or a reel entering a reel case or the tape
// wall, a sack entering the drone. On a peer where another player carries that prop, the prop is a mirror its
// carrier's pose stream moves, and the device there would take it natively, outside every lane: it seats a prop its
// carrier still holds, and a port's insert drops THIS peer's own grab. So the rule sits on the prop: a prop a remote
// player carries is taken by its carrier's world alone, and every other peer refuses the device's overlap entry for
// it at the script gate, as it does for a device a remote player carries; the carrier's take reaches them through the
// device's own lane or the relayed destroy. The floppy slot's entry is
// the same rule for a disc in transit (coop/interactables/floppy_slot_entry); the desk's back panel entry is
// refused the same way in coop/interactables/physmods_sync, whose panel lane carries the flag. One such take is
// not refused yet, since no lane carries what it changes and a refusal would only hide that: the sack the drone
// puts on. Game thread.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::mirror_slot_entry {

// Register the watches on every device's overlap entry. Idempotent. Session install.
void Install(coop::net::Session* session);

// Settle the watches' registration and say once whether they are live. Game thread, per frame.
void Tick();

// [dev] the drills' read: the entries this peer refused.
uint64_t Refused();

// The session's counts. Session end.
void OnDisconnect();

}  // namespace coop::mirror_slot_entry

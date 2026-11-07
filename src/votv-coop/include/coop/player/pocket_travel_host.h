// coop/player/pocket_travel_host.h -- the host's half of a pocket trip (coop/player/pocket_travel.h).
//
// The host owns the location table, parks an away slot (not world-ready, its puppet hidden and
// held where it stood, its pose and world lanes stopped, its profile's place frozen at the
// departure spot), relays pose and voice only between slots in the same level, answers a return
// with a second save transfer for the admitted slot, and refuses its own trips while hosting.

#pragma once

#include <cstdint>

#include "coop/player/pocket_travel.h"

namespace coop::net { class Session; struct PocketTravelPayload; }

namespace coop::player::pocket_travel_host {

void Install(coop::net::Session* session);
void Tick();
void OnSessionStart();
void OnDisconnectForSlot(uint8_t slot);

// A client's PocketTravel, from the reliable drain (coop/dispatch). `senderSlot` is the
// connection's slot; the payload never names another.
void OnPocketTravel(uint8_t senderSlot, const coop::net::PocketTravelPayload& p);

// The host's own lib_C::loadLevel to a pocket or the main world, at the body's entry.
ue_wrap::script_gate::Verdict OnLocalLoadLevel(coop::player::pocket_travel::Pocket dest,
                                               const wchar_t* level, void* author);

// Is this slot away in a pocket (or on its way back, until its return's ready edge)? Any thread.
bool IsSlotAway(uint8_t slot);

// Late join: every away slot's location row to one slot (a slot in the main world needs no row: a
// joiner's table starts there).
void SendLocationsToSlot(uint8_t slot);

// ---- the session's questions (any thread: the host's net thread asks them per message) --------
// The state is one atomic byte per slot.

// No world lane runs on this slot's link, either way: it is away, or it asked to come home and its
// return's ready announce has not arrived yet. The send paths give it only the kinds that touch no
// world (session_lanes.h IsAwaySendableKind) and the receive edge takes only those from it
// (IsAwayReceivableKind).
bool IsSlotIsolated(uint8_t slot);
// Away and not on its way back: its world-ready announce is refused (it has no main world), and two
// such slots in the same pocket get each other's pose and voice.
bool IsSlotParked(uint8_t slot);
// Was the return armed for this slot (a ReturnRequest taken, its ready edge not yet run)? The save
// transfer re-arms only such a slot (coop::save_transfer::HostArmReturnForSlot).
bool IsSlotReturning(uint8_t slot);
// The net thread received a ClientWorldReady from `slot`: if its return was armed, the slot's link
// carries world lanes again from this message on (the game thread's ready edge runs one drain later,
// as the relay's eligibility stamp does at a join). True when the slot may be stamped relay-eligible
// (it is not parked).
bool NoteWorldReadyReceived(uint8_t slot);

// The net thread received a PocketTravel from `slot`, before the inbox: an Enter isolates the slot at
// once, so what its client sends after the edge (its world's teardown) is already refused at the
// receive edge rather than one game-thread drain later. The game thread judges the edge in full
// (OnPocketTravel) and puts the state back if it refuses it.
void NoteTravelReceived(uint8_t slot, const coop::net::PocketTravelPayload& p);

// The host's answer to a slot's first world-ready announce (coop/session/world_ready_answer), before
// it is marked world-ready: a returning slot is back in the main world (its row, told to every
// client), and every first announce, a join's too, is sent the rows of the slots that are away.
// False when the announce must be refused: the slot is parked in a pocket and has no main world.
bool OnFirstWorldReady(uint8_t slot);

}  // namespace coop::player::pocket_travel_host

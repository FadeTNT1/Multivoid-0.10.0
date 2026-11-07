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

// Is this slot away in a pocket (or on its way back)?
bool IsSlotAway(uint8_t slot);

// Late join: every slot's location row to one slot.
void SendLocationsToSlot(uint8_t slot);

}  // namespace coop::player::pocket_travel_host

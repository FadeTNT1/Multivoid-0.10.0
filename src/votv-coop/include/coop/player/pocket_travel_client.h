// coop/player/pocket_travel_client.h -- the client's half of a pocket trip
// (coop/player/pocket_travel.h).
//
// The client raises the edges: Enter at the loadLevel decision (its carried state captured, the
// host told, its world's co-op state released before the travel), Arrived when the pocket is up
// (its carried state put back over the stale slot's), and the return, where the pocket's own way
// home is turned into a second save transfer and world load for the admitted slot. While away it
// runs only its own body's streams, voice and chat, and shows only the puppets in the same level.

#pragma once

#include <cstdint>

#include "coop/player/pocket_travel.h"

namespace coop::net { class Session; struct PlayerLocationPayload; }

namespace coop::player::pocket_travel_client {

void Install(coop::net::Session* session);
void Tick();
void OnSessionStart();

// The host's PlayerLocation row, from the reliable drain.
void OnPlayerLocation(const coop::net::PlayerLocationPayload& p);

// This client's lib_C::loadLevel to a pocket or the main world, at the body's entry.
ue_wrap::script_gate::Verdict OnLocalLoadLevel(coop::player::pocket_travel::Pocket dest,
                                               const wchar_t* level, void* author);

// Is this client away (in a pocket, or travelling to or from one)? World lanes stand down while
// it is.
bool LocalAway();

}  // namespace coop::player::pocket_travel_client

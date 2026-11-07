// coop/player/pocket_travel_client.h -- the client's half of a pocket trip
// (coop/player/pocket_travel.h).
//
// The client raises the edges: Enter at the loadLevel decision (its carried state captured, the
// host told, its world's co-op state released before the travel), Arrived when the pocket is up
// (its carried state put back over the stale slot's), and the return, where the pocket's own way
// home is turned into a second save transfer and world load for the admitted slot
// (coop/player/pocket_return_client). While away it runs only its own body's pose, voice and chat,
// and shows only the puppets in the same level.

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

// This client's lib_C::loadLevel to a pocket or the main world, at the body's entry (inside the
// script body: nothing here dispatches; the work that does is posted).
ue_wrap::script_gate::Verdict OnLocalLoadLevel(coop::player::pocket_travel::Pocket dest,
                                               const wchar_t* level, void* author);

// Is this client away (in a pocket, or travelling to or from one)? Any thread.
bool LocalAway();

// Do the world lanes stand down this tick? True while entering or away, and on the way home until
// the host's world is up in place of the one the client left. Game thread.
bool WorldLanesStandDown();

// A new local pawn on a trip that is not the host's world's (the pocket's, or the old world's
// before the travel): the join's placement and the start-point teleport must not move it. Game
// thread.
bool SkipNewPawnPlacement();

// The registry reaper's menu guard: the current world is a pocket level this client went to, not the
// menu, so the session stays up. Logs once per trip. Game thread.
bool KeepSessionInOtherWorld();

// A run-ending menu travel while away (a death in the pocket, a run-ending there): the travel is
// cancelled and the player is brought home alive instead. `author` is the travel's author. True
// when this lane took it (the caller cancels and revives nothing); false leaves the seam's own
// verdict. Inside the script body: nothing here dispatches.
bool OnRunEndWhileAway(void* author);

// The local pawn died while away and the death arc is not armed (the pump's flee would end the
// session): bring the player home instead. Once per trip. Game thread.
void OnUnarmedDeathWhileAway();

// The way home finished (coop/player/pocket_return_client): ok, back in the main world with the
// world-ready announce sent; not ok, the transfer or the load failed and the session ends with a
// named reason. Game thread.
void OnReturnFinished(bool ok, coop::player::pocket_travel::Reason why);

// Where THIS machine's player is: the host is always in the main world; a client is where its trip
// has it (the pocket from the decision to go until the decision to come home). Any thread.
coop::player::pocket_travel::Pocket ViewerLocation();

// HOST: the location table's edges as feed lines ("<nick> went to the poop dimension", "<nick> came
// back"); a client words them from PlayerLocation itself. Cheap: one compare per slot. Game thread.
void TickHostLocationFeed();

}  // namespace coop::player::pocket_travel_client

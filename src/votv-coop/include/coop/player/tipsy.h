// coop/player/tipsy.h -- a player who drank wine or beer is tipsy for a few minutes, and every screen shows it beside their
// nameplate (ui/hud: a few wobbling bubbles).
//
// The drink. prop_snack_C.eat takes the food (addFood) and returns early when the player is full; only past that does it
// count a use and call snackEaten. A script-body watch on snackEaten (any class: a subclass may override it) therefore sees
// exactly the drinks that went down, and the class name picks the alcohol (prop_snack_beer_C, prop_snack_wine_C). The
// drink runs on the drinker's own machine -- the hand item is theirs, and a world prop's use is granted by the host and
// then run by the client -- so the local player is credited; a drink the host runs inside an acting-slot scope for a
// client's puppet is not the host's, and is skipped.
//
// Each drink adds five minutes and the total is held to fifteen. The state is a deadline on this machine's clock; what
// crosses the wire is one bit of the player's own pose (PoseSnapshot stateBits, kStateBitTipsy), which the host relays
// as it relays the rest and which a late joiner receives with the next pose. Game thread.

#pragma once

namespace coop::tipsy {

// Each frame, game thread: register the watch until the script-body gate takes it, then say so once.
void Tick();

// Is the local player tipsy now. Any thread (a deadline compare).
bool LocalTipsy();

// The session ended: the state is cleared (a new session starts sober).
void OnDisconnect();

}  // namespace coop::tipsy

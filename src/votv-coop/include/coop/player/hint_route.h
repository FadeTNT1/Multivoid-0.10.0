// coop/player/hint_route.h -- an in-game hint reaches the player it is for, not the host.
//
// The game raises every hint (the info-icon toast, bottom right) through one function, lib_C::addHint, on the
// machine whose Blueprint ran. The host runs many Blueprints on a client's behalf -- the client's intents, its
// Kerfus and Kerfur Omega commands, the drone it sent -- so their hints painted on the host's screen, and the
// client who acted saw nothing: "Kerfur cannot find broken server" stacked on the host once per client press.
//
// The host watches lib_C::addHint at the script-body gate and answers each call:
//   1. Inside an acting-slot scope (coop/net/acting_slot.h) -- the reliable drain of a client's message and every
//      intent queue's served head open one -- the hint is that slot's: refused on the host and sent to the slot
//      (HintRoute), whose own lib_C::addHint shows it with the same text, icon and duration.
//   2. With no scope, a hint whose raiser was set going by a client and now runs on its own -- a Kerfus or Kerfur
//      Omega serving a client (coop/creatures/served_player), the drone a client sent selling at the sell point
//      (Credit) -- goes to that client, unless the chain began at the host's own player (the host pressed it).
//   3. Everything else -- the host's own presses, world events, save and load notices -- stays on the host as the
//      game shows it. No hint is broadcast: a world-wide notice reaches each client through the state its own game
//      paints the hint from, as before. An editor-only (`debug`) hint is left to the game, which shows nothing.
// The same text to the same slot within a short window is sent once, so a burst of identical presses is one hint.
// A client never routes: it shows what arrives and runs its own hints as the game does, and a routed hint its own
// game raised in that window too (a body both machines ran) is not shown a second time.
// Gameplay/network layer (principle 7); the engine through ue_wrap::votv_lib and ue_wrap::drone.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct HintRoutePayload;
}  // namespace coop::net

namespace coop::hint_route {

// Keep the session and register the lib_C::addHint watch. Idempotent. Game thread.
void Install(coop::net::Session* session);

// Drive the watch live and say so once. Every pump tick. Game thread.
void Tick();

// HOST: `actor`'s later hints, raised with no acting scope (its tick, a timer, a latent resume), are `slot`'s;
// slot 0 (the host) or a null actor forgets the credit. One credit per actor, the last one wins. Game thread.
void Credit(void* actor, uint8_t slot);

// CLIENT: a hint the host routed here. Shows it through this machine's lib_C::addHint. Game thread.
void OnRouted(const coop::net::HintRoutePayload& payload);

// A peer left: its credits and its repeat window go.
void OnPeerLeft(uint8_t slot);

// Session end: every credit, window and counter.
void OnDisconnect();

}  // namespace coop::hint_route

// coop/props/shrimp_tray_sync.h -- a shrimp tray's portions are the host's.
//
// A shrimp tray (prop_food_shrimp_C) hands out a portion on its action 4 and on its playerHandUse_RMB: the body takes
// one from `shrimps`, redraws the tray (upd), spawns its `shrimpDrop` above it and, at the last one, spawns the empty
// tray (shrimp_e) and destroys itself. Run on a client, that took a portion from the client's copy alone and made
// births the drop-intent lane refuses, so the trays and the shrimp differed between players.
//
// On a client both presses on a SHARED tray (a world prop, not one in a hand) are cancelled at the script-body gate
// and sent to the host as ShrimpDispenseIntent with the tray's key and which press it was; a press the client cannot
// send runs on its own copy as it would with no lane. The host resolves the key to a tray, refuses a sender that is
// not alive, out of reach, a tray another peer holds and an empty tray, and runs the game's own verb once on its copy
// with the sender's puppet as the player. The drop, the empty tray and the tray's destroy are the host's births and
// death and ride the ordinary prop lifecycle. The count rides the food state (coop/props/prop_food_state) as its
// `portions`: the host marks the tray when its upd runs, and a client writes the count and runs the game's upd.
//
// A tray in a player's hand is that player's own item (coop/player/hand_item): its RMB runs where it is pressed, and
// what it spawns is authored by that player (coop/props/prop_spawn_authoring asks HandDispenseRunning), so the drop
// and the empty tray reach the host as the player's births. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct ShrimpDispenseIntentPayload;
}  // namespace coop::net

namespace coop::props::shrimp_tray_sync {

// Cache the session; register the three watches (the press, the hand use, the redraw) and the food lane's portions
// hooks once. Re-entered by the install pump.
void Install(coop::net::Session* session);

// The watches' settle; HOST: each sender's queued presses, judged and run at a bounded rate.
void Tick(coop::net::Session& session);

// HOST: a client's press from the wire, its format checked by the dispatcher. Queued for Tick.
void OnIntent(coop::net::Session& session, const coop::net::ShrimpDispenseIntentPayload& p, uint8_t senderSlot);

// The dispatcher refused a press (its size, its key or its route): counted.
void CountDropped();

// CLIENT: is this peer's own RMB on a tray in its hand running now? Asked by a finish-spawn seam to tell the drop
// and the empty tray it spawns from the world's own births.
bool HandDispenseRunning();

// Net disconnect: the queues and counters go, with one summary line.
void OnDisconnect();

}  // namespace coop::props::shrimp_tray_sync

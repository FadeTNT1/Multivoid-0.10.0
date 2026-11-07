// coop/interactables/cargolift_sync.h -- the cargo lift (cargolift_C), one lift for every peer.
// Overview: docs/devices.md. Gameplay/network layer (principle 7); the engine is reached through ue_wrap/actors/cargo_lift.
//
// A lift's button (actionOptionIndex, action 4) flips the lift's `dir` and starts a latent loop that moves its car along
// Z on the peer that pressed, so each peer's lift moved alone and the car a rider stood on was not where the others saw
// it. The lift is the host's: a CLIENT's press is refused at the script-body gate and sent (CargoLiftIntent), naming the
// lift by key and the action; the host tests the sender's reach and an idle lift and runs the game's own press. Every
// press the host's lift starts, the host's own or one it ran for a client, goes to every client as CargoLiftState op
// Press, and each client replays the game's own press on its copy (outside the gate, by a replay flag), so its own loop
// moves the car and the riders ride along. When the host's trip ends, op Rest names the end it came to: a client whose
// car is not there (a press it missed, a trip that began late) is brought there once its own trip ends, or after a
// grace. Late join: the host sends every lift's Rest at the joiner's world-ready.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct CargoLiftIntentPayload;
struct CargoLiftStatePayload;
}  // namespace coop::net

namespace coop::cargolift_sync {

// Watch the lift's press and cache the session. Idempotent.
void Install(coop::net::Session* session);

// Settle the watch; HOST: run one queued press a tick a sender and send a Rest when a trip ended; CLIENT: apply the
// Rests that waited for a car still moving.
void Tick(coop::net::Session& session);

// HOST: a client's press (event_dispatch_intent).
void OnIntent(coop::net::Session& session, const coop::net::CargoLiftIntentPayload& payload, uint8_t senderSlot);

// CLIENT: the host's press or rest (event_dispatch_intent).
void OnState(coop::net::Session& session, const coop::net::CargoLiftStatePayload& payload, uint8_t senderSlot);

// HOST: every lift's resting place to the joiner at its world-ready.
void OnPeerWorldReady(int slot);

// Session end: the tables and counters go, said.
void OnDisconnect();

}  // namespace coop::cargolift_sync

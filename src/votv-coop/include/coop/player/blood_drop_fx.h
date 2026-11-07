// coop/player/blood_drop_fx.h -- a bleeding player's blood drops, fallen on every peer (ReliableKind::BloodDropFx).
// Gameplay/network layer (principle 7); the engine is reached only through ue_wrap::blood_drops and the script gate.
//
// A bleeding player's blood-loss effect (effect_bloodLoss_C) lives on that player's own peer: its dropTimer traces
// down from the local camera and hands a drop to the gamemode's bloodDecalMaster_C.addParticle, whose falling
// particle leaves a grime_blood2 decal on the floor. Nothing of it reached the other peers, so the blood under a
// bleeding player showed on that player's screen alone. Here the bleeding peer watches addParticle while its
// dropTimer runs, sends each drop's start point, velocity, size and type, and every other peer runs the game's own
// addParticle with them: the same drop falls from the same point and stains the floor there too. Only the bleeding
// drops travel: a creature's, a pipe's or a gib's blood comes from bodies each peer already runs or mirrors.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct BloodDropFxPayload;
}  // namespace coop::net

namespace coop::blood_drop_fx {

// Cache the session. Called each time the install fanout runs.
void Install(coop::net::Session* session);

// Per tick: register the watches until they take, send this peer's queued drops, the minute's summary. Game thread.
void Tick();

// A BloodDropFx the receive edge accepted. `senderSlot` is the transport's sender, -1 when unknown; on the host a
// client's message must name that client's own player. Game thread.
void OnReliable(const coop::net::BloodDropFxPayload& payload, int senderSlot);

// Is `payload` (`len` bytes) well formed: a non-zero origin, 1..kBloodDropFxMax drops, every value finite and
// bounded, a known type. The receive edge asks before OnReliable.
bool WellFormed(const coop::net::BloodDropFxPayload& payload, int len);

// Session end: the queue and the counts are dropped.
void OnDisconnect();

}  // namespace coop::blood_drop_fx

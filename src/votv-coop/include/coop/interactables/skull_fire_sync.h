// coop/interactables/skull_fire_sync.h -- the animal skull's effect reaches every player.
//
// A microwave's loop calls microwave(self) on what is inside it; on prop_wendskull_C that waits 1 s, rolls 10 % and
// calls ignite(60), which lights the skull, plays a 2D ambience, spawns ten wendussy_C and a wendigodreamer_C and ends
// with progressAchievement(microskull) and its own destroy. Every one of those reads player 0 or the local camera, so
// the effect reaches only the machine that ran it. The microwave is the host's (coop/interactables/microwave_sync):
// a client's loop and its microwave() calls are cancelled, so the roll is made once, on the host.
//
// The host watches the skull's ignite -- from the roll or any other way it is lit -- and sends the skull's prop key
// on MicrowaveState with the skull-fire flag, once per skull. A client resolves the key to its copy of the skull and
// runs the skull's own ignite there, so the effect is made on its machine against its own player, with its own sound.
// The host's own run is the original; it is never sent its own cue. A copy's end-of-effect destroy is marked as the
// wire's, so no peer is told of it. A joiner mid-effect misses it. Game thread.

#pragma once

namespace coop::net {
class Session;
struct MicrowaveStatePayload;
}  // namespace coop::net

namespace coop::skull_fire_sync {

// Cache the session and register the host's watch on the skull's ignite, once. Re-entered by the install pump.
void Install(coop::net::Session* session);

// The watch's settle, until it has settled.
void Tick();

// CLIENT: the host's cue, a MicrowaveState with the skull-fire flag, past the format check.
void OnCue(const coop::net::MicrowaveStatePayload& p);

// Net disconnect: the tables and counters go.
void OnDisconnect();

}  // namespace coop::skull_fire_sync

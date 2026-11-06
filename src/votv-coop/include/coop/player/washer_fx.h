// coop/player/washer_fx.h -- the pressure washer's spray, seen and heard by every peer
// (ReliableKind::WasherFx). Gameplay/network layer (principle 7); the engine is reached only through
// ue_wrap::water_shooter and the engine wrappers.
//
// The washer fires from its own tick on the peer that holds it: one water shot a frame, each a
// peer-local prop that cleans where it lands (what it cleans crosses on the cleaning lanes). Nothing
// of that was visible elsewhere. The firing peer now sends a start when its washer starts firing, a
// keep every 100 ms with up to four of its shots' landing points (sampled from the splash each shot
// spawns), and a stop when the trigger is let go, the tank runs dry or the washer leaves its hand.
// Every other peer shows the jet (eff_waterThrower) at the firer's puppet along its aim, the hiss
// (wade_Cue, spatialized) there, and a splash with its sound where each sampled shot landed. It never
// runs the washer's firing tick, which would fire real shots and clean a second time. A receiver stops
// on a stop of the same hold, when the firer dies, on its slot's disconnect, and after 600 ms without
// a keep; that last also seeds a joiner, who picks a firing washer up from the next keep.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct WasherFxPayload;
}  // namespace coop::net

namespace coop::washer_fx {

// Cache the session. Called each time the install fanout runs.
void Install(coop::net::Session* session);

// Per tick: the local washer's edges and keeps, the splash observer's arming, and every remote
// washer's jet and hiss. Game thread.
void Tick();

// A WasherFx the receive edge accepted. `senderSlot` is the transport's sender, -1 when unknown;
// on the host a client's message must name that client's own player. Game thread.
void OnReliable(const coop::net::WasherFxPayload& payload, int senderSlot);

// Is `payload` (`len` bytes) well formed: a known firing value, a landing count within the cap,
// every coordinate finite and bounded, a non-zero origin. The receive edge asks before OnReliable.
bool WellFormed(const coop::net::WasherFxPayload& payload, int len);

// The slot left: its washer stops showing here.
void OnSlotDisconnected(uint8_t slot);

// Session end: every remote washer stops, the counts are said.
void OnDisconnect();

}  // namespace coop::washer_fx

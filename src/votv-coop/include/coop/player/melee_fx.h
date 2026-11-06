// coop/player/melee_fx.h -- the sound and particles of a melee swing, its hits and a held item's knocks, heard and seen
// by every peer. The game makes them inside the swinger's own graph, so only the swinger had them. Two seams on the
// local machine turn each into a message: mainPlayer_C::attack (the sounds its own graph plays, and the hit it returns)
// and comp_physicsImpact_C::impactDamage on a prop this player holds (a damaging knock, a break), sent before the body
// runs. A client sends MeleeFxRequest; the host judges it and publishes MeleeFx to every other client and plays it; the
// host's own go straight to every client. A message names its assets by path and its place by coordinates, so a break
// plays after its prop is gone. A world container's sounds stay with coop/props/prop_impact_fx; a creature the host
// parks answers a hit with its own feedback on the host. No late-join replay. Game thread unless stated; docs/players.md.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct MeleeFxPayload;
}  // namespace coop::net

namespace coop::melee_fx {

// Cache the session; take the swing and contact watches and the sound observers once per process (they act on either
// role, each callback asking the session). Called each time the install fanout runs.
void Install(coop::net::Session* session);

// The watches' settle, and the close of a swing scope a faulted body left open. Game thread.
void Tick();

// Is `payload`, `len` bytes of a MeleeFx or MeleeFxRequest, well formed: its kind and effect bits known, every float
// finite and bounded, its text printable ASCII within the cap and spelled as an asset of the class it names, its ids
// non-zero. The receive edge asks it before either handler below.
bool WellFormed(const coop::net::MeleeFxPayload& payload, int len);

// HOST: a client's request, judged and published.
void OnRequest(int senderSlot, const coop::net::MeleeFxPayload& payload);

// CLIENT: the host's effect, played.
void OnFx(const coop::net::MeleeFxPayload& payload);

// Net disconnect: the pace, the ids and the once-per-session latches go.
void OnDisconnect();

}  // namespace coop::melee_fx

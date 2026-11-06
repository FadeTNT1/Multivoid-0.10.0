// coop/items/poster_sync.h -- the poster, rolled and placed, is the host's. docs/props.md, "Posters", has the why.
//
// ROLLED (prop_poster_C, a prop): the prop lane carries it; its record (type, file name) lands here as a
// prop_record_gates applier, the record then setTex, since its own loadData does not redraw. A picture picked on it
// republishes the record; a client's is an intent the host takes only from a live sender in reach.
//
// PLACED (poster_C, an actor_save_C no other lane carries): the host names each by its actor_save key (minted when
// it has none), watches setPoster and liveness, and sends PosterState (class, transform with scale, look; or gone).
// A client's put, take-down (action 7) and custom picture are cancelled and sent as PosterIntent; the host checks
// life and reach and runs the verb on its own copy. A put is asked under a request id with the held poster's look (a
// poster in a hand is no world prop), answered to that client alone as PosterState (accepted with the canonical key,
// or refused), and finished on the client once by the game's own put(), whose wall poster takes the canonical key;
// a put the client cannot finish is given back. A client mirrors PosterState, adopting save-loaded posters by key.
// Late join: the save carries the keys; the ready edge sends every placed poster and an end marker, and the joiner
// destroys any it was not named. A custom image is a file name only; a peer without the file draws the preset.
// Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PosterIntentPayload;
struct PosterStatePayload;
}  // namespace coop::net

namespace coop::poster_sync {

// Cache the session, register the watches and the rolled poster's record gates once. Re-entered by the install pump.
void Install(coop::net::Session* session);

// The watches' settle; HOST: the first scan of the placed posters, the queued intents, the changes and removals
// published; CLIENT: the parked states retried.
void Tick(coop::net::Session& session);

// HOST: a joiner's world is up: every placed poster's state to it, then the snapshot's end.
void OnPeerWorldReady(int slot);

// HOST: a client's verb. CLIENT: the host's state.
void OnIntent(coop::net::Session& session, const coop::net::PosterIntentPayload& p, uint8_t senderSlot);
void OnState(const coop::net::PosterStatePayload& p);

// The dispatcher's format checks, and its drop count.
bool WellFormed(const coop::net::PosterIntentPayload& p);
bool WellFormed(const coop::net::PosterStatePayload& p);
void CountDropped();

// Net disconnect: the tables, queues and counters go, with one summary line. The placed posters stay where they are.
void OnDisconnect();

}  // namespace coop::poster_sync

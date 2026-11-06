// coop/props/water_fill_sync.h -- the water in a world bucket (prop_bucket_C `height`) is the host's, and every peer
// sees it rise.
//
// A faucet_C and a prop_shower_C each trace down from their nozzle in their own ReceiveTick and, over a bucket, raise
// its `height` (15 and 5 a second), clear its `empty` flag and call its upd(). Every peer runs that tick on its own
// copy, so a bucket under a running tap filled on each machine by itself: a level that differed by a peer's frame
// rate, and one that never agreed with the host's after a pick-up, a nudge or a pour.
//
// The host owns the level. On a client both taps' ticks are watched at the script-body gate: the buckets' water is
// read before the tick and put back after it, so the tap's splash, sound and the rest of its tick still play and
// only its write to a shared bucket is dropped. The host's copy fills as it always did, and the host sends the level
// of every world bucket whose water changed (BucketFillState: the prop key, `height`, `empty`) about twice a second
// while it changes and once more when it settles, and to a joiner at its ready edge for every bucket holding water. A
// client lands it by writing `height` and `empty` -- and `height_interp` where the bucket does not simulate physics
// here, since its tick eases that field and a bucket at rest does not tick -- and calling the bucket's own upd().
// A client's own pour (the bucket's tick lowers `height` by the tilt) is the bucket's own tick on that machine and
// stays local, as it was; the next fill the host sends carries the host's level.
//
// NOT covered, on purpose: a bucket (or watering can) in a player's HAND. holdingActor_main is the hotbar hand item,
// which is player state, not a world entity (docs/players.md): sink_C.playerUsedOn fills that item on its holder's
// machine, the other peers see a display-only mirror that carries no water, and the contents reach them in the
// bucket's save record when the holder drops it. There is nothing here for the host to publish or validate, so the
// sink's press is left to run where it is made. Likewise a bucket dipped into a lake (its own tick's water-float
// term) stays on the machine that dips it. Game thread.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::water_fill_sync {

// Cache the session, register the scan-hub index of the buckets and the two tap-tick watches. Idempotent; called
// every net-pump tick.
void Install(coop::net::Session* session);

// Settle the watches; HOST: send the buckets whose water changed.
void Tick(coop::net::Session& session);

// HOST, at a joiner's world-ready edge: every world bucket holding water to that slot.
void OnPeerWorldReady(int slot);

// CLIENT, a BucketFillState from the host (the dispatcher has checked the sender and the length).
void OnState(const coop::net::BucketFillStatePayload& p);
bool WellFormed(const coop::net::BucketFillStatePayload& p);
void CountDropped();

// Session teardown: the index, the ledger and the counters go, with one summary line.
void OnDisconnect();

}  // namespace coop::water_fill_sync

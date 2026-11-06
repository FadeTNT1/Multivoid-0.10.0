// coop/interactables/surface_clean_sync.h -- the dirt of the kitchen sink and the shower cubicle (sink_C /
// prop_shower_C `clean`, 0 filthy .. 1 spotless), on ReliableKind::SurfaceCleanState. Gameplay/network layer
// (principle 7): the wire, the sponge watch, the apply, the Key->actor index, the deferred retry and the connect
// snapshot; the engine is reached only through ue_wrap::surface_clean.
//
// The sponge's cleanSponge body on each class raises `clean` by strength * a small constant, calls upd() (which
// clamps it and paints it onto the cube's material) and, at 1, credits the wiping player's own cleaning
// advancement. Nothing lowers it, so the model is the base window's mirrored: a max register. While connected,
// each peer sends a surface's `clean` when its own cleanSponge raised it -- a script-gate PRE/POST watch on both
// bodies, keyed by the surface's Aactor_save_C::Key, the same key family the appliance lane names the sink's and the
// shower's tap bit by. A receiver resolves the surface by Key, deferring and retrying while it has not streamed in,
// and applies MAX(local, wire): a live edge can only make a surface cleaner, so two peers wiping at once converge
// without oscillation, and no host authority is needed. The apply writes the field and calls upd() -- never
// cleanSponge -- so it plays no sound, moves no wipe cursor and credits no advancement, and is never heard back as a
// wipe. The host's relay carries a client's wipe to the other clients. When a joiner's world is ready the host sends
// each surface's current value with adopt=1, applied AS SENT so the joiner takes the host's world even where its own
// save was dirtier or cleaner -- which is why an adopt counts only from the host.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KeyedScalarPayload;
}  // namespace coop::net

namespace coop::surface_clean_sync {

// Register with the scan hub (the Key->actor index) and put the wipe watch on both cleanSponge bodies. Idempotent;
// called every net-pump tick. Stores the session pointer. Game thread.
void Install(coop::net::Session* session);

// Receiver entry: a SurfaceCleanState packet arrived, its payload already copied and range-checked by event_feed.
// Applies MAX(local, clean) for a live wipe (adopt==0) or the value as sent for a host snapshot (adopt==1, slot 0
// only), deferring if the instance has not streamed in. Game thread.
void OnReliable(const coop::net::KeyedScalarPayload& payload, uint8_t senderPeerSlot);

// HOST-only: the current `clean` of every indexed surface to the client `peerSlot` whose world is ready, adopt=1.
// Called from the connect replay. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per tick: settle the watches, retry the deferred applies (throttled). Game thread.
void Tick();

// Session teardown: clear the pending applies and the sent ledger.
void OnDisconnect();

}  // namespace coop::surface_clean_sync

// coop/player/item_activate.h -- unified item-activation sync. First instance: the
// flashlight. The world light cone is a component of the player pawn, not of the flashlight
// item actor; the puppet (also a pawn orphan) has the same light at the same offset, so
// driving its light is the full implementation. Sender path: a per-tick poll of the local
// player's beam (on when the flashlight bool is set and a flashlight is equipped; its
// intensity, cones, reach, colour and profile) sends the payload to peers on any change,
// off included, whatever route changed it; an echo-suppression flag keeps the receiver's
// apply from bouncing back. Receiver path: ApplyToPuppet drives the puppet's light through
// the engine wrapper. The flashlight_log config flag logs every send, not only on and off;
// off by default.
#pragma once

#include <cstdint>

namespace coop::net { class Session; struct ItemActivatePayload; }

namespace coop::item_activate {

// Install: cache the session pointer the sender polls with (null disables broadcasting) and the
// flashlight's class hash. Safe to call every pump tick.
void Install(coop::net::Session* session);

// Receiver-side apply: a peer reported their flashlight state or cone changed. `puppetActor`
// is the pawn orphan we already spawned for that peer. The payload carries the state, the
// intensity, the outer and inner cone angles and a mode byte; the class hash distinguishes
// the flashlight from future item payloads (a mismatched class no-ops). `senderPeerSlot` is
// the resolved sender slot (the feed's element-registry lookup, with the packet's slot as
// the fallback when the mirror is not yet established); used by the click sound's per-peer
// state and per-peer logging. Game thread only.
void ApplyToPuppet(void* puppetActor, const coop::net::ItemActivatePayload& p,
                   uint8_t senderPeerSlot);

// The connect-time replay receiver entry: if `puppetActor` is valid, applies immediately.
// Otherwise stashes the latest payload in a per-peer slot keyed by `senderPeerSlot`, drained
// on the next TickConnect once the puppet has spawned. Latest wins: a newer packet overrides
// a still-pending older one. Game thread only.
void ApplyToPuppetOrDefer(uint8_t senderPeerSlot, void* puppetActor,
                          const coop::net::ItemActivatePayload& p);

// Per-slot connect-time flashlight replay. Snapshots the local pawn's current flashlight
// state and sends it to one specific peer. Called from the harness's per-slot connect edge;
// a single aggregate broadcast that fired once would skip late joiners. `peerSlot` is the
// players-registry slot of the newly joined peer (1 and up on the host; 0 on a client).
// No-op with a log if the local flashlight is off (the receiver's puppet default is already
// off). Game thread only.
void QueueConnectBroadcastForSlot(int peerSlot);

// The host-relay late-joiner replay. Where QueueConnectBroadcastForSlot replays this peer's
// own item state to a newly joined slot, this replays every other existing peer's current
// item state to the joiner, so it converges to the live world (the packet is edge-triggered,
// so a flashlight already on before the joiner arrived would otherwise stay dark until the
// owner next toggles). Host only (the relay hub holds the per-peer cache); no-op on clients.
// `newSlot` is the freshly connected client's slot. Game thread only.
void ReplayPeerStatesToSlot(int newSlot);

// The per-tick worker. Polls the local beam and sends a change (the sender), and drains the
// per-peer pending applies (walks the puppet registry; each
// slot with a pending payload and a valid puppet is applied and cleared). Cheap when nothing
// is pending. Game thread only.
void TickConnect();

// The disconnect hook: clears the pending broadcast and every per-peer pending apply. The
// stashed state belonged to the dead session; replaying it onto the next session's peers
// would be wrong.
void OnDisconnect();

// The per-slot variant of OnDisconnect for mid-session peer drops. Clears only that slot's
// pending apply, if any; without this a stale flashlight payload from a departed peer would
// re-apply when a new peer reuses the slot. Safe to call when nothing is pending.
void OnDisconnectForSlot(int peerSlot);

// FNV-1a 32-bit hash of a wide string, the item class hash (cross-peer stable, simple to
// inline, no table). Exposed so the receiver can compute the expected hash to compare
// against the payload.
uint32_t HashClassName(const wchar_t* utf16);

// The autonomous-test entry point. Flips the pawn's flashlight flag directly and invokes our
// send path with the new state; the LAN flashlight test drives toggles with it without
// relying on the blueprint graph (calling the blueprint's update via reflection runs the
// graph but does not toggle the flag, since the blueprint is gated on input state we cannot
// fake). Returns the new state. Game thread only.
bool DebugForceToggle(void* mainPlayer);

}  // namespace coop::item_activate

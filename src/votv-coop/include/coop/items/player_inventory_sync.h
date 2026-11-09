// coop/items/player_inventory_sync.h -- the per-player profile (coop/items/player_profile.h:
// what a player carries, wears and holds, their vitals, where they stood), host-persisted at <mod data dir>/coop_players/<host save>/<guid>.json. The GUID is derived from the
// key that peer PROVED at admission (coop/net/peer_identity.h), never from a value it sent.
//
// A joiner's world is built from a capture of the HOST's save object, and the player's record in
// it (saveSlot.betterObjectsData) carries the host's inventory component, so without a
// substitution every joiner loads the host's items, and the host's status effects ride along as
// records of its world, which the join hook resets out of the joiner's copy. In session order: the
// host sends a joining peer its profile while that peer is still pre-world; the client arms the
// substitution on the save object and carries it out the moment its player has loaded the host's
// items (ue_wrap/actors/inventory_component), or arms the game's own starting kit on a first join;
// the client streams its profile back as it changes (and at once after an admin's order,
// SendProfileNow), only from a world that was built from one; the host holds each complete, changed
// blob by GUID and cuts the held profiles to disk when its own world is saved, at no other moment
// (coop/player/player_profile_store.h says why). Nothing held and no file IS the first-join test.

#pragma once

#include <cstdint>

namespace coop::net { class Session; struct BlobChunkPayload; }

namespace coop::player_inventory_sync {

// Cache the session pointer and register the two save seams. Called at every session start.
void Install(coop::net::Session* session);

// ---- transport and host persistence ----

// Bidirectional PlayerInventoryBlob receiver (event_feed -> here); branches by role:
//   * HOST receiving from a CLIENT slot (1..): one chunk of that client's inventory STREAM.
//     Reassembles (per-sender) and hands a complete blob to the profile store, which holds
//     the newest one per GUID.
//   * CLIENT receiving from the HOST (slot 0): one chunk of the host's ON-JOIN apply blob.
//     Reassembles and, on completion, deserializes + stashes it as the pending
//     per-player inventory (HasPendingApply()), to be written into the save object by the
//     SaveObjectReadyHook before the world materializes.
// Game thread.
void OnReliable(const coop::net::BlobChunkPayload& p, uint8_t senderPeerSlot);

// ---- the live apply on join: host to client, then the save-object-ready hook ----

// HOST: send peer `peerSlot` its per-player profile (the one the store holds, else its file,
// FNV-verified with a .bak fallback; a profile with no items, the game's starting kit, when there is
// neither -- never another player's inventory). Chunked to that ONE slot over PlayerInventoryBlob. Called from the host tick the
// moment the slot is connected and its GUID has arrived, so the blob lands in the joiner's
// pre-world window. No-op off the host or when the peer's GUID hasn't arrived. Returns true iff
// the blob was actually enqueued -- the caller (HostPersistTick) latches "sent" ONLY on true, and retries next tick on a channel-busy refusal
// (else the connect-edge refusal was never retried -> the client never got its inventory). Game thread.
bool SendInventoryToSlot(int peerSlot);

// CLIENT: true once the host's on-join apply blob has arrived + deserialized (the join boot waits
// on this before loading the world, so the SaveObjectReadyHook always has the data). Game thread.
bool HasPendingApply();

// CLIENT: a count that rises with every chunk of the host's apply blob that arrives. A join wait
// that sees it move knows the host is delivering (a large blob is paced over many seconds); only
// silence is a failure. Game thread.
uint64_t ApplyBlobChunksTaken();

// CLIENT: the next save object to come ready belongs to a join, so the hook may substitute the
// profile into it (or empty the host's items out of it when none arrived), and the hook resets the
// host's status effects out of it before either. One-shot, consumed by
// the hook and cleared on disconnect; the join boot calls it right before each world load. Every
// other load in the process -- a later Host-with-save above all -- is left alone. Any thread.
void BeginJoinApply();

// CLIENT: where the join's world appearance puts this player, taken once: a later body in the same
// session is a respawn, which belongs at the start point. ProfilePose fills x, y, z and yaw with where
// the applied profile says the player stood. AtHost (the dev row join_at_host) leaves the player where
// the transferred save put it, the host's own position. StartPoint on a first join, for a player who
// left dead, with no profile applied, and for every later body. Game thread.
enum class JoinPlacement : uint8_t { StartPoint, ProfilePose, AtHost };
JoinPlacement TakeJoinPlacement(float& x, float& y, float& z, float& yaw);

// Per-slot disconnect (host): re-arm the on-join push for that slot. The leaver's profile stays
// held by GUID. Client: no-op. Game thread.
void OnDisconnectForSlot(int peerSlot);

// Aggregate disconnect: the client clears its send-dedup and any pending apply. The host keeps
// what the store holds -- on a host this edge is the last client leaving. Game thread.
void OnDisconnect();

// CLIENT: send this client's profile now, on the stream's path, past the stream's hash and cadence
// checks; a send restarts the stream's 30 s cadence. False when the stream is shut (not connected,
// or this world was not built from a host profile, so the host keeps the one it holds), when the
// items do not read, or when the transport refused or the profile is too large. Game thread.
bool SendProfileNow(coop::net::Session* s);

// HOST: how many client profile blobs have been assembled and handed to the store so far: one per complete stream
// that came from a sender with a GUID (a complete stream dropped for want of one is not counted), an identical blob
// included (the store skips those, so its own state does not move). A reader waits on the count rising. Game thread.
uint32_t ProfilesReceived();

// HOST: how many writes of the hosted world's save object have been seen and had their profiles cut to disk (the
// last step of the save seam; a write the seam refuses is not counted). A reader waits on the count rising. Game thread.
uint32_t WorldSavesWritten();

// ---- a pocket trip (coop/player/pocket_travel_client) ----
//
// A pocket level is a full map load on the client, restored from the client's in-memory save object,
// which is the join's (a client never saves): natively the player would arrive with the inventory it
// joined with. The client is the one author of what it carries, so it captures that state at the
// trip's edges and puts it back itself; the host only holds the flushed copy for its saves.

// HOST: push slot `slot` its held profile again exactly as at a join: the per-slot pushed latch is
// re-armed, so the next 1 Hz pass sends it the profile the store holds. Logged. Game thread.
void HostRearmApplyForSlot(uint8_t slot);

// CLIENT: capture what the local player carries now (items, hands, equipment, vitals) as the trip's
// carried state. A read that fails, or an inventory the void-death keep calls collapsed, keeps the
// previous capture. Returns the item count of the capture held after the call (carried, hands and
// equipment slots), or -1 when none is held. Game thread, outside a script body (the read dispatches).
int ClientCaptureCarried(const char* why);

// CLIENT: send the profile now, made of the captured carried state, with `departure` (x, y, z cm;
// pitch, yaw, roll deg) as the player's place, past the stream's hash and cadence. False when nothing
// is captured, this world was not built from a host profile, or the transport refused. Game thread.
bool ClientFlushCarried(const float departure[6]);

// CLIENT: put the captured carried state back on the local player at the next world's own load (its
// player load or its loadObjects return), the join's substitution path. Game thread.
void ClientArmCarriedApply();
enum class CarriedApply : uint8_t { Idle, Armed, Applied, Failed };
CarriedApply ClientCarriedApplyState();
// CLIENT: put it back now on the local player, when no load event ran in time (the settle fallback).
// False with no local player or nothing armed. Game thread.
bool ClientApplyCarriedNow(const char* when);

// CLIENT: the way home re-runs the join's load. Re-arms the one-shot join apply (BeginJoinApply) and
// the one-shot join placement (TakeJoinPlacement), so that load puts the carried state on the player
// and places it at `departure`. The carried state is this client's own capture when one is held (the
// host's re-push of its held copy is then not used: the client authored both, and its capture is the
// newer); without one, the boot waits for the host's re-push. Idempotent. Game thread.
void ClientRearmForReturn(const float departure[6]);

// CLIENT: forget the trip's carried state and any return arm (a finished trip, a session start).
void ClientForgetCarried();

// CLIENT: the last standing pose the profile stream sampled (not falling, ragdolled, dead, seated or
// held by a script), when it was taken at most `maxAgeMs` ago: x, y, z cm and yaw deg. False when none
// is that recent. Game thread.
bool ClientRecentStandingPose(int64_t maxAgeMs, float& x, float& y, float& z, float& yaw);

// Per-tick: the client's outbound inventory stream, or the host's on-join push, by role. It
// also carries a one-shot read-verify self-test (ini inventory_selftest=1) that reads the local
// saveSlot inventory a few seconds after world-up and logs what it found; that part is a no-op
// unless the flag is set. Game thread.
void Tick();

}  // namespace coop::player_inventory_sync

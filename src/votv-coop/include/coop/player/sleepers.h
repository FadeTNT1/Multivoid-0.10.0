// coop/player/sleepers.h -- offline players who stay in the world, asleep (experimental, `sleepers.enabled`).
//
// When a client leaves a hosted world (on the dedicated server every player is a client), the host lays a SLEEPER down
// where that player stood: their own body, lying flat on the floor, carrying their inventory. Every peer shows it,
// a joiner included; any other player can loot it (E) in the game's own container screen; what is taken or put
// is written back into the player's profile on the host, so the owner wakes up, when they rejoin, where the sleeper
// lay and with what is left. The sleeper is keyed by the player's stable id (the GUID the profile store keys by) and
// is written beside the profiles when the host's world is saved, so it survives a host restart with the world.
//
// The body is a puppet (an unpossessed mainPlayer_C, ue_wrap/actors/puppet) wearing the player's skin, rotated onto
// its side and set on the floor height the host traced -- a deterministic pose, no physics, nothing that can stand it
// back up. Its loot is a hidden crate (prop_container_crate_C) at the same spot, spawned on every peer as a
// display-only prop (never enrolled, never saved, collision off), whose inventory component is a world storage of
// address kind Sleeper on the existing storage lanes: the host's contents publish (coop/props/storage_contents) and a
// client's take or put asked of the host with its escrow and commit (coop/props/storage_act). The host holds the
// player's profile items as they were when the sleeper lay down and rebuilds them from the crate's rows whenever they
// change (ue_wrap/actors/storage_inventory RebuildProfileItems).
//
// The host itself never sleeps: a listen host that leaves ends the session. A kick or a ban lays no sleeper down and
// takes an existing one away (the player's profile keeps what the looting left). Gameplay layer (principle 7).
// Game thread.

#pragma once

#include "coop/net/protocol.h"
#include "coop/text/utf8_codec.h"
#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop { class RemotePlayer; }
namespace coop::net { class Session; }
namespace coop::roster_ledger { struct Row; }

namespace coop::sleepers {

void Install(coop::net::Session* session);  // the loot press seam; retried until mainPlayer_C is loaded
void Tick();                                // build, pose watch, the host's write-back and its saved list
void OnClientDisconnect();                  // a client's session ended: its sleepers go
void OnSessionEnd();                        // any role: every sleeper and record of this session dropped

// ---- online sleepers ----
// An online player at rest (coop/player/sleep_sync: resting in a bed or a sleeping bag, or dreaming) is shown as a
// sleeper on every other peer: their puppet laid flat on the bed surface -- or the floor under a sleeping bag -- where
// they rest, by the same measured pose as an offline sleeper, settled once per rest. While it lies the puppet's normal
// drive is suspended (RemotePlayer::SetDrivesSuspended), so the lying pose is the only writer; a pose watch every 2 s
// puts it back only if it drifted. They stand up when an awake read holds for 1 s (shorter flips are ignored) or at once
// on a ragdoll. Called for each live puppet after its own tick (coop/player/puppet_drive). Looting an online sleeper is
// not in this build: `sleepers.loot_online` is off and reserved for it.
void DriveOnline(int slot, coop::RemotePlayer& puppet);
// Whether the puppet of `slot` lies as an online sleeper on this peer.
bool IsLyingOnline(int slot);
// The fixed point a lying online sleeper's nameplate floats at (the body's centre, lifted), not the animated head bone,
// which swings with the head. False when the slot is not lying.
bool OnlinePlateAnchor(int slot, ue_wrap::FVector& out);

// ---- host edges ----

// A client's occupancy of `slot` ended (the roster row transition), BEFORE its puppet is destroyed: lay its sleeper.
void OnPlayerLeaving(int slot, const coop::roster_ledger::Row& outgoing);
// The host is kicking or banning this player: no sleeper for this departure, and one already lying goes.
void NoteRemovedByHost(const std::string& guid);
// The player is about to be handed their profile (a rejoin). False while a loot transaction on their sleeper is still
// held (it settles first, and none starts meanwhile): the caller asks again. True once the sleeper's loot has been
// written back and the sleeper is gone, or there was none. The profile the join then reads IS the sleeper's.
bool OnOwnerJoining(const std::string& guid);
// Host: the sleeper `id` is closing (its owner is coming back); storage_act refuses it new transactions.
bool LootClosing(uint32_t id);
// Host: this player has just woken from a sleeper and its record is being put on them; a profile they stream until
// that is answered is dropped, so nothing they loaded locally overwrites the record.
bool HoldsProfileOf(const std::string& guid);
// Host: the answer to the wake's restore (coop/items/inventory_restore).
void OnWakeAck(uint8_t slot, const coop::net::InventoryRestoreAckPayload& ack);
// Every sleeper to a joiner at its ready edge, ahead of the world storages' seed.
void QueueConnectBroadcastForSlot(int slot);
// The world was gathered into its save object / its save was written beside the profiles.
void OnWorldGathered();
void OnWorldSaveWritten();

// ---- the wire ----
void OnStateChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// ---- the loot's storage address (coop/props/storage_contents) ----
// The sleeper id whose crate is `ownerActor`, or 0.
uint32_t LootIdOfOwner(void* ownerActor);
// The crate's inventory component of sleeper `id` on this peer, or null.
void* LootStorage(uint32_t id);
// Every built sleeper's id.
void LootIds(std::vector<uint32_t>& out);

// ---- the nameplates (coop/player/nameplate) ----
struct PlateAnchor {
    ue_wrap::FVector head{};
    char nick[coop::text::kNickBufBytes] = {};
    bool lootable = false;   // the local player is in reach of it and aiming at it: "E  Loot"
};
size_t PlateAnchors(PlateAnchor* out, size_t max);

// How many sleepers this peer holds (built or not), for a log line or a test.
size_t Count();

}  // namespace coop::sleepers

// coop/items/inventory_restore.h -- the host puts a backed-up inventory back on a live player.
//
// A lost inventory used to be recoverable only with the world closed: the backup profiles
// (coop/player/player_profile_store) are files, and a player's inventory is read from them at the join. This is the
// live route, for a host's admin: /restoreinventory <player> [backup], or the Players list of the dev menu.
//
// The host reads the backup, checks its magic and checksum (the store does) and that it parses as a profile with an
// inventory in it, and sends the inventory's bytes to that one client as InventoryRestoreBlob. The client keeps a
// rollback copy of what it holds (a profile file in its mod data folder, `inventory_rollback/`), replaces its three
// item arrays through the game's own restore on the game thread (vitals, position and everything else of the player
// stay as they are), refreshes the equipment and answers InventoryRestoreAck with the item counts before and after.
// The host logs the request, the answer and the counts under [RESTORE]. The host's own player is restored locally,
// the same way, with no message. Nothing is restored to a dead player, and the profile stream sends the result to the
// host like any other change of the inventory. Game thread.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::inventory_restore {

// Cache the session (idempotent; every net-pump tick).
void Install(coop::net::Session* session);

// Sweep half-assembled blobs and expire requests nobody answered. Every net-pump tick.
void Tick();

// HOST: restore `backup` (see player_profile_store::ReadBackup; empty is the newest full profile) to the player in
// `slot` (0 is the host's own player). `reply` is the line for the admin. True when the restore was applied (slot 0) or
// sent (a client; its answer is logged when it arrives).
bool Request(int slot, const std::string& backup, std::string& reply);

// HOST: put `items` (a profile's inventory component bytes) on the client in `slot`, which must be in the world, on
// the same lane and with the same rollback copy and answer as Request. `label` names the source in the log. The
// answer is handed to coop::sleepers::OnWakeAck: this is the wake of a sleeper, whose record replaces whatever the
// player holds. False when the slot is not ready, a restore to it is still unanswered, or the send was refused.
bool SendItems(int slot, const std::vector<uint8_t>& items, const std::string& label);

// CLIENT, a chunk of an InventoryRestoreBlob (any role may be handed one; only a client taking it from slot 0 acts).
void OnBlob(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot);

// HOST, a client's InventoryRestoreAck.
void OnAck(const coop::net::InventoryRestoreAckPayload& ack, uint8_t senderSlot);

// HOST, `slot` left: a restore still waiting for its answer is dropped, or a rejoin within the minute the request is
// kept would be told "still waiting" and refused.
void OnSlotLeft(int slot);

// Session teardown.
void OnDisconnect();

}  // namespace coop::inventory_restore

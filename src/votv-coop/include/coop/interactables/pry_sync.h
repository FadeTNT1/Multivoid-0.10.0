// coop/interactables/pry_sync.h -- a crowbar's pry, the Pry kind of coop/interactables/tool_action.
//
// A pry is a prying crowbar (pryingCrowbar_C, ue_wrap/devices/pryable) stuck in its pryable: its begin() reveals it
// and takes the held crowbar, its owner's mouse bends it, its end() gives the crowbar back and, on a pry, runs the
// pryable's crowbarOpen. The owner's begin is announced with the crowbar's place and lives; its bend is sent at a
// pace while it moves; its end names how it ended. A receiver shows the crowbar stuck and bent on its own copy of
// the child with its tick and collision off, and hides it with the game's end(false, false, true), which gives
// nothing back. On a client the end of its own pry that changes the world -- the pry (its fling and the pryable's
// crowbarOpen), the dropped crowbar, the strain's snap -- is cut to that same cleanup at the script-body gate, and
// the host runs the real one on its copy, once: end(true, false, false), end(false, true, false) or snap(). The
// crowbar taken back into the owner's hand runs where it is taken, an inventory of that player's own. The child is
// named on the wire by its door's key for a pryable door, else by its portable identity. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct ToolActionPayload;
}  // namespace coop::net

namespace coop::pry_sync {

void Install(coop::net::Session* session);
void Tick(coop::net::Session& session);
void OnIntent(coop::net::Session& session, const coop::net::ToolActionPayload& p, uint8_t senderSlot);
void OnState(coop::net::Session& session, const coop::net::ToolActionPayload& p);
void ConnectReplayForSlot(coop::net::Session& session, int slot);
void OnPeerLeft(coop::net::Session& session, uint8_t slot);
void OnDisconnect();

}  // namespace coop::pry_sync

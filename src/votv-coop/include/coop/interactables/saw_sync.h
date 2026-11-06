// coop/interactables/saw_sync.h -- a hacksaw's cut, the Saw kind of coop/interactables/tool_action.
//
// A cut is the hacksaw in a player's hand (prop_hacksaw_C, ue_wrap/actors/hacksaw) with a heavy prop as its
// `sawingProp`: its tick raises `complete` and at 1 spawns the scraps, breaks and destroys the prop. The owner's
// begin names the prop by its key; its progress is sent at a pace; its completeSaw ends it. On a client the tick that
// would finish the cut is cancelled at the script-body gate -- the bar holds just short of full -- and the end goes
// to the host, which measures the cut itself (the mesh's volume and the material's density, as the saw does), runs
// the end once on its copy of the sender's saw (coop/player/hand_item's mirror), and the scraps, the spill and the
// prop's destroy are host births and death. A receiver plays the saw's sound at the cut while it lasts. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct ToolActionPayload;
}  // namespace coop::net

namespace coop::saw_sync {

void Install(coop::net::Session* session);
void Tick(coop::net::Session& session);
void OnIntent(coop::net::Session& session, const coop::net::ToolActionPayload& p, uint8_t senderSlot);
void OnState(coop::net::Session& session, const coop::net::ToolActionPayload& p);
void ConnectReplayForSlot(coop::net::Session& session, int slot);
void OnPeerLeft(coop::net::Session& session, uint8_t slot);
void OnDisconnect();

}  // namespace coop::saw_sync

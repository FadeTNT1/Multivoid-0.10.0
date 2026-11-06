// coop/interactables/toolbox_sync.h -- a repair kit's mend of a burst water pipe, the Toolbox kind of
// coop/interactables/tool_action.
//
// The kit (prop_toolbox_C, ue_wrap/actors/toolbox) held on a pipe it can fix takes the pipe as its `fixingActor`,
// reads the pipe's fix time into `time` (the power tool in the other hand makes it a fifth), raises `complete` on its
// tick and at 1 asks the pipe for toolboxFix: on true it takes one of its `fixes`, resets and, with none left, is
// destroyed. The owner's begin names the pipe by its portable key and carries the kit's time and uses; its progress is
// sent at a pace; its completeSaw ends it. On a client the tick that would finish the mend is cancelled at the
// script-body gate and the end goes to the host, which checks the pipe is still burst, the sender holds a kit, is in
// reach, had the time and has a use left, then commits the mend once on its pipe (coop/world/basement_pipe_sync sends
// the flag) and tells the sender to run its kit's own end -- the use taken, the reset, the message, the destroy when
// none are left -- so the kit's uses stay in the sender's own inventory record. A receiver plays the kit's repair
// sound at the pipe while a mend lasts. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct ToolActionPayload;
}  // namespace coop::net

namespace coop::toolbox_sync {

void Install(coop::net::Session* session);
void Tick(coop::net::Session& session);
void OnIntent(coop::net::Session& session, const coop::net::ToolActionPayload& p, uint8_t senderSlot);
void OnState(coop::net::Session& session, const coop::net::ToolActionPayload& p);
void ConnectReplayForSlot(coop::net::Session& session, int slot);
void OnPeerLeft(coop::net::Session& session, uint8_t slot);
void OnDisconnect();

}  // namespace coop::toolbox_sync

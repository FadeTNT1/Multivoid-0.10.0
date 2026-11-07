// coop/save/save_transfer_internal.h -- what save_transfer.cpp lends its sibling
// save_transfer_return.cpp and nothing else. Internal to coop/save (src tree, not include/).

#pragma once

namespace coop::net { class Session; }

namespace coop::save_transfer::detail {

// The session save_transfer_return.cpp sends through, from save_transfer's Install.
void InstallReturn(coop::net::Session* session);

// Clear `peerSlot`'s one-shot world-save request latch (MV-H33), so its next request is served. Only
// HostArmReturnForSlot calls it, for a slot on its way home from a pocket. Game thread.
void ClearRequestLatchForReturn(int peerSlot);

}  // namespace coop::save_transfer::detail

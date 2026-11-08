// coop/props/dig_fill.h -- a CLIENT's dirt ball landing in a hole. Private to coop/props; dig_sync.h has the family.
//
// The hole's own box overlap destroys the dirt ball that enters it and the hole with it. A hole every peer mirrors is
// the host's, so the overlap is refused on a client, and the ball a client throws is its own (the trash lane carries no
// dirt ball a pile did not make): nothing ever told the host. Here the refused overlap becomes a Fill intent, and the
// ball is spent when the host's answer says the hole is gone. The ball a peer's own mirror shows is never sent: its
// owner's overlap fills the hole where it was thrown. Game thread.

#pragma once

#include "ue_wrap/core/script_gate.h"

#include <cstdint>

namespace coop::net {
class Session;
}

namespace coop::dig_fill {

void Install(coop::net::Session* session);

// The watch on the hole's box overlap: a client's is always refused; one by a dirt ball of its own sends the intent.
ue_wrap::script_gate::Verdict OnHoleOverlapPre(const ue_wrap::script_gate::Call& c);

// The host's ack for request `reqId`. True when the id was a fill's (spent or refused, said in the log); false when it
// is not ours and the caller keeps it.
bool OnAck(uint32_t reqId, bool ok);

// CLIENT, each tick: a fill nobody answered is forgotten.
void Tick(uint64_t nowMs);

// Net disconnect.
void Reset();

}  // namespace coop::dig_fill

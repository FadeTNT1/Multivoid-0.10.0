// coop/props/dig_out.h -- the dig lane's outbound queue, shared by dig_sync and dig_fill. Private to coop/props.
//
// One queue: the host's state (slot -1: every client, or one slot's) and a client's intents (to the host). A blob the
// link refuses stays at the head and goes again next tick under a fresh number. Game thread.

#pragma once

#include <cstdint>
#include <vector>

namespace coop::net {
class Session;
}

namespace coop::dig_out {

// Queue one packed message. slot -1: every client (a host's state) or the host (a client's intent).
void Enqueue(int slot, std::vector<uint8_t>&& bytes);

// Send what the link takes now (a handful per tick).
void Drain(coop::net::Session& s);

// Net disconnect: the queue goes.
void Clear();

}  // namespace coop::dig_out

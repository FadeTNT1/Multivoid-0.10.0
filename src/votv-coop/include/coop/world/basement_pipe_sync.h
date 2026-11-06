// coop/world/basement_pipe_sync.h -- a burst water pipe's damage is the host's, and every peer hears and sees the spray.
//
// A burstWaterPipe_C holds `damaged`: while it is set, its tick feeds the basement flooder's addWater (the flood's
// inflow), its particle system sprays and its audio hisses. The flag is set by its state(true) (reached by addDamage:
// the basement drain's random burst, any hit on it) and cleared by state(false) (reached by a toolbox repair), and
// read back by its save. Every peer ran those on its own; with the flood's water writers refused on a client, a
// client's own burst or repair changed nothing the host's inflow felt.
//
// The host owns the flag. On a client the pipe's state and addDamage bodies are refused at the script gate for the
// game's own callers, so no client bursts or mends a pipe by itself; the host's state() is seen at its return and sent
// as BasementPipeState (the pipe's portable identity and the flag), and every pipe's flag goes to a joiner at its
// ready edge. A client lands one by calling the pipe's own state(), so the spray, the sound and the tick follow; a
// flag for a pipe that has not streamed in waits for it. A client's hit on a pipe is not yet sent to the host (it
// does nothing here). The repair itself is coop/interactables/toolbox_sync. Game thread.

#pragma once

#include "coop/net/protocol.h"

#include <string>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::basement_pipe_sync {

// Cache the session and register the state and addDamage watches (idempotent; every net-pump tick).
void Install(coop::net::Session* session);

// Settle the watches; CLIENT: land the flags that waited for their pipe.
void Tick(coop::net::Session& session);

// HOST, at a joiner's world-ready edge: every pipe's flag to that slot.
void OnPeerWorldReady(int slot);

// CLIENT, a BasementPipeState from the host (the dispatcher has checked the sender and the length).
void OnState(const coop::net::BasementPipeStatePayload& p);
bool WellFormed(const coop::net::BasementPipeStatePayload& p);
void CountDropped();

// The pipe with this portable wire key in the running world, or null. Game thread.
void* FindPipe(const std::wstring& key);

// The pipe's portable wire key, empty when it has none. Game thread.
std::wstring KeyOf(void* pipe);

// While one of these lives, this lane's watches let the pipe's state and addDamage through: a repair the host judged,
// run on a client's own copy, is the host's word.
class ScopedApply {
public:
    ScopedApply();
    ~ScopedApply();
    ScopedApply(const ScopedApply&) = delete;
    ScopedApply& operator=(const ScopedApply&) = delete;
};

// Session teardown.
void OnDisconnect();

}  // namespace coop::basement_pipe_sync

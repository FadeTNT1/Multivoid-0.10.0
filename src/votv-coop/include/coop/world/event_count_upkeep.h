// coop/world/event_count_upkeep.h -- a client's event count, kept honest.
//
// The gamemode's activeEvents is a refcount: lib_C::setEvent(true) adds one and appends the sender, the sender's own
// setEvent(false) takes both back. A sender that dies without its false (a mirror a host destroy retires, an actor
// whose false lives in a timeline's finish) leaves the count above the senders still alive, and while it stands the
// pause menu, the quit and the sleep are refused (lib_C::getEvent). The host's own count is the game's; this keeps a
// client's:
//   - it names the holders: every begin and end on a client is logged with the sender class (rate-limited), and a
//     count that stays above 0 says which senders hold it, and for how long, every 30 s;
//   - it takes back what a destroyed mirror leaves (ReleaseLeft, at the two sites that destroy a world-actor mirror);
//   - it reconciles once a second: a count that stands above its live senders for three polls in a row is lowered
//     to them, with setEvent(false) on the gamemode as the context, one call per stale registration (ReclaimDead).
// A live sender is never touched: a per-viewer creature, a copy the client's own player started and a host event's
// replay end by their own Blueprint. Game thread only.

#pragma once

#include <string>

namespace coop::net {
class Session;
}

namespace coop::event_count_upkeep {

// How many registrations `sender` holds in the event registry, read before it is destroyed.
struct Held {
    int         copies = 0;
    std::wstring className;
};
Held Snapshot(void* sender);

// After `sender` was destroyed: any registration its Blueprint did not take back (the registry still names it) is
// taken off the count, and said. `site` names the caller for the log.
void ReleaseLeft(void* sender, const Held& before, const char* site);

// Lower the count to its live senders now; the number of registrations taken back. Also the sleep refusal's route.
int ReclaimDead(const char* why);

// CLIENT: the once-a-second look (held-count log, vanished senders, the stale-count rule). Called every pump tick.
void Tick(coop::net::Session* session);

// CLIENT, after a lib_C::setEvent body ran: the begin or end of `sender`, logged.
void OnClientSetEvent(void* sender, bool active);

// Drop the tracked senders.
void OnDisconnect();

}  // namespace coop::event_count_upkeep

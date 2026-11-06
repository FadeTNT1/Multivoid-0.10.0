// coop/props/vent_census.h -- the vent-cover census: what every prop_vent_C looks like on this peer,
// logged once when the world is ready and once 30 s later, on host and client alike.
//
// A report of "the vent covers are gone" is one of three things: the actor is absent (never made,
// or destroyed), present and hidden (a join mirror never revealed, or a visibility flag lost), or
// present and displaced. The host's census and the client's census, read side by side, settle which
// without a guess. Diagnostic only: it reads, never writes.
//
// One summary line, then one line per cover (capped), each tagged `[VENT-CENSUS]`:
//
//   [VENT-CENSUS] <when> role=<host|client> world-gen=<n> covers=<n> visible=<n> hidden=<n> held-by-join=<n> unread=<n>
//   [VENT-CENSUS]   key='<key>' eid=<n> static=<0|1> frozen=<0|1> actorHidden=<0|1|?> meshVisible=<0|1|?>
//                   meshHiddenInGame=<0|1|?> collision=<-1..3> loc=(<x>,<y>,<z>) join=<held|-|n/a>
//
// `<when>` is `world-ready` or `+30s`. `join=held` means the join sweep's deferred-hide window still
// holds that mirror hidden (coop/element/mirror_defer); `-` means it does not; `n/a` on the host.
// Game thread only.

#pragma once

namespace coop::vent_census {

// Drive from the pump tick. `ready` is the peer's "world is ready" term (a host: its world is up; a
// client: it has announced world-ready). The first tick with `ready` set after a world appears logs
// the world-ready census; the first tick 30 s on logs the second. A new world re-arms both.
void Tick(bool isHost, bool ready);

}  // namespace coop::vent_census

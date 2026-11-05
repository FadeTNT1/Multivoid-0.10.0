// coop/world/church_sync.h -- the Stolas church (outsideChurch_C, new in 0.10.0) as host-authoritative state.
//
// The church is level-placed on every machine. Its event is one refcount: harvestedHerb counts a herb
// (each herb the church spawned in the rain fires it when it is picked or wilts at the rain's end), the
// eighth sets angry and calls lib_C::setEvent(true); only lightningStrike ends it, reached from the
// lightning trigger's end-overlap by a mainPlayer_C while angry, ten seconds after the strike.
//
// The host runs the church whole. A remote player's puppet is a mainPlayer_C, so its leaving the trigger
// strikes the host's church natively; that strike's camera focus (focusPlayer_C turns player 0) and
// slow motion (global time dilation) belong to the striking player, so on the host they are refused when
// a puppet struck. A client runs none of the church's state-writing verbs (no herbs of its own, no count,
// no anger, no strike, no break, no lightning at its player, no quartz sweep), so its own event count
// never registers the church; it mirrors the four save fields the host's 1 Hz poll sends and runs the
// church's own presentation refreshes (updBroken, updEyes), the pair loadData runs. The strike's flash
// rides the lightning lane, the debris props the prop lane. Game thread throughout.

#pragma once

namespace coop::net {
class Session;
struct ChurchStatePayload;
}  // namespace coop::net

namespace coop::church_sync {

// Cache the session and register the script-gate watches (idempotent; retried by the install fanout).
void Install(coop::net::Session* session);

// Per pump tick, throttled to 1 Hz: the host polls the church and broadcasts a change, both roles settle
// the watches, the host retires a refused focus actor, a client drains a state that arrived early.
void Tick();

// HOST, at a joiner's world-ready edge: the church's current state to that slot, unconditionally.
void QueueConnectBroadcastForSlot(int slot);

// CLIENT: the host's church state (event_feed, game thread).
void OnReliable(const coop::net::ChurchStatePayload& payload);

// Session end: the cached church, the poll baseline, the scopes.
void OnDisconnect();

}  // namespace coop::church_sync

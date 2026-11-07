// coop/world/fire_sync.h -- the host's fire on every peer: the burning patches (fire_C) and a fuel puddle's flame
// (grime_gasoline_C). Gameplay and network layer (principle 7); the engine through ue_wrap::fire, the script gate and
// the world-actor lane.
//
// A fire_C is an actor_save_C a cigarette on a sleeper's bed, a broken bottle bomb, a spreading flame on a flammable
// floor or a save's load mints under EX_CallMath, and it is no player's deploy (sdk_profile_names.h says why the save
// actor lane leaves it out), so no lane carried it and a client never saw the host's floor burn. It rides the
// world-actor lane (coop/world/world_actor_sync): the host enrols each fire at its begin-play, and a scan-hub sweep
// enrols one that began play before the lane could see it (a save's load before hosting), with the fields its
// begin-play reads in the birth blob; the lane's own dead-retire carries its end. A client's mirror is visuals only:
// before its begin-play its fuelSpeed and ignite chance are written 0, so its component never burns out on its own, and
// its component's spreadFire is cancelled, so it lights and damages nothing here; the host's destroy puts it out.
//
// A client never authors fire: a fire_C begin-play on a client that is not the lane's own mirror is cancelled and the
// actor destroyed (a cigarette, a bomb, a spreading flame, a save's load all mint one there), the scan-hub sweep removes
// one that slipped past, and a fuel puddle's ignite is cancelled there unless the lane runs it.
//
// A fuel puddle is a grime decal, named across peers by grime_sync's key. Its ignite adds a flammable component on the
// decal itself (not a birth): the host's ignite is sent on GrimeState as kGrimeOpFireOn with the fuel, and its end
// (put out, burnt out, the decal destroyed) as kGrimeOpFireOff, read off the host's tracked puddles twice a second; a
// joiner gets every burning puddle at its world-ready. The client runs the puddle's own ignite, parks the component as
// above, and runs its extinguish on the off. A key the client's index has not reached waits up to two minutes.
//
// Late join: the world-actor lane's connect snapshot carries the live fires; QueueConnectBroadcastForSlot the puddles.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::fire_sync {

// Store the session and register the scan-hub sweep. Idempotent. Game thread.
void Install(coop::net::Session* session);

// Per pump tick: register the watches until each is live (each says so once), the host's puddle reads, the client's
// waiting puddles. Game thread.
void Tick();

// Teardown: the tracked puddles, the waiting ones, the counters (after a summary line). Game thread.
void OnDisconnect();

// HOST: every burning fuel puddle to a joiner at its world-ready, as kGrimeOpFireOn. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// The world-actor lane's birth blob for fire_C. IsFireActor / IsFireClassName pick the class; FillBirth writes the
// host's fields (a version byte and eight floats); SeedMirror writes them, parked, into a mirror between its deferred
// begin and its finish, or parks the class defaults when the blob is empty or unusable (said). Game thread.
bool IsFireActor(void* actor);
bool IsFireClassName(const std::wstring& cls);
void FillBirth(void* actor, unsigned int eid, uint8_t* blob, size_t cap, uint8_t& len);
void SeedMirror(void* actor, unsigned int eid, const uint8_t* blob, size_t len);

// CLIENT: the host's flame op for the puddle `key` names (grime_sync has judged the sender is the host): lit with
// `fuel`, or put out. Game thread.
void OnGasolineFlame(const std::wstring& key, bool on, float fuel);

}  // namespace coop::fire_sync

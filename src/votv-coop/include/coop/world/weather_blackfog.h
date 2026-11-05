// coop/world/weather_blackfog.h -- the black fog, owned by the host, mirrored on a client.
//
// AblackFog_C is a global weather event: mainGamemode_C::spawnBlackFog spawns it at the identity
// transform and its whole state is one float, `a` (a ramp-in, a 180 s hold counted down in `a`
// itself, a 1 s fade). Its gameplay is spawnghost, a 40 s timer that mints eyer_C stalkers on the
// navmesh around the origin. The host keeps its own roll; its fog's presence, `a`, `spd` and a fade
// bit ride WeatherState (coop/world/weather_fog stamps and applies them through here). A CLIENT's
// own birth is destroyed by coop/world/weather_event_births; this lane spawns the client's copy of
// the host's (echo-flagged past that catch), snaps `a` each pulse, and refuses the copy's
// spawnghost, so the eyers a client sees are the host's on the owner-entity lane. A joiner gets it
// in the weather seed. docs/events-and-weather.md is the player-facing page.

#pragma once

namespace coop::net { struct WeatherStatePayload; }

namespace coop::weather_blackfog {

// Set the role and register the client's spawnghost refusal (a name watch, once; it self-gates on
// a connected client session at fire time). Called from weather_fog::Install every call.
void Install(bool isHost);

// HOST: true iff a live blackFog_C exists. Constant time when one is cached (an index revalidate);
// a by-class find at most once per 5 s while none is cached. Game thread.
bool HostHasBlackFog();

// HOST: stamp kBlackFogActive / kBlackFogFading into out.flags2 and the ramp into blackFogA /
// blackFogSpd from the cached host black fog (HostHasBlackFog must have run this pass). Game thread.
void ReadHostState(coop::net::WeatherStatePayload& out);

// The host's fade bit as last computed, for the fog-edge detector's bit word. Game thread.
bool HostFading();

// CLIENT: the host has one -> ensure one client copy (spawned at the identity transform, seeded
// before its BeginPlay) and snap `a` by phase; the host has none -> fade a holding copy out over
// its own 1 s, else destroy it. Game thread; called from weather_fog::ApplyFromHost.
void ApplyFromHost(const coop::net::WeatherStatePayload& payload);

// True while ApplyFromHost is mid mirror-spawn; weather_event_births lets that birth through.
bool MirrorEchoActive();

// Session end: drop the caches and the client gate. A live copy is left to its own clock.
void OnDisconnect();

}  // namespace coop::weather_blackfog

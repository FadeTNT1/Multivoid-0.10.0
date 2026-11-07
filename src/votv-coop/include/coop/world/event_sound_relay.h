// coop/world/event_sound_relay.h -- a host event actor's positional one-shots, played on every client.
//
// Some event actors make their whole sound from their own graph: GameplayStatics::PlaySoundAtLocation and
// SpawnEmitterAtLocation calls at their place (the vent crawler's footsteps along the crawl and the vent it breaks,
// the vent knocker's seven knocks).
// On a client every such actor is a mirror with the playing bodies refused, so the pose crossed and the sound did
// not. This relay is generic: a native pre seam on PlaySoundAtLocation and a post seam on SpawnEmitterAtLocation,
// armed on the host only while an enrolled npc or world actor of a listed source class lives (ue_wrap's
// kPositionalCueSourceClasses; the relay polls for one itself),
// take each call whose calling graph is such an actor's, and send it as EventSoundCue: the asset by its full
// name, the place, the volume, the pitch and the attenuation. A client plays it once through the same engine
// calls; its own copy never plays one, so nothing echoes. Host authoritative, cosmetic; game thread.
//
// The same lane carries the light flicker (op kFlick): ticker_flickerer_C calls trigger_lightRoot_C::flick on
// every light group, bypassing runTrigger and so the light group lane. A script-body watch on flick refuses a
// client's own call and marks the host's; the host sends one cue per tick that flicked, and a client calls flick
// on every light root of its announced world.

#pragma once

namespace coop::net {
class Session;
struct EventSoundCuePayload;
}  // namespace coop::net

namespace coop::event_sound_relay {

// Cache the session and install the two seams, disarmed (retried each pump tick until they take). Game thread.
void Install(coop::net::Session* session);

// HOST: arm or disarm the seams by whether a source actor lives (polled four times a second), then send what the
// seams captured since the last tick. Game thread.
void Tick();

// Format check of a received cue. Any thread.
bool WellFormed(const coop::net::EventSoundCuePayload& p);

// CLIENT: play a host cue (event_feed, game thread).
void OnCue(const coop::net::EventSoundCuePayload& p);

// Teardown: disarm, drop the queue.
void OnDisconnect();

}  // namespace coop::event_sound_relay

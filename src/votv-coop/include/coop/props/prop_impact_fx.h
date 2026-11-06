// coop/props/prop_impact_fx.h -- a world container's hit, damage and break cues, heard by every peer.
//
// comp_physicsImpact_C plays its physics material's cues (physMat_SoundsAndParticles_C: sound_impact, sound_damaged,
// sound_destroyed; a supply crate's physSnd_wood_crate holds impact_crate_Cue, damage_wood_Cue and break_crate_Cue)
// inside impactDamage, and a container's impactDamage runs on the host's copy alone (coop/props/crate_break). So the
// host watches that body before it runs: on a container whose component the call damages or breaks (the component's own
// arithmetic, prop_damage_gate::ImpactOutcomeOf), or one a client's swing or contact the host runs now strikes without
// damage, it sends PropImpactFx to every client, before the break's PropDestroy on the same lane. The receiver plays its
// own copy's cue of that kind at the point (coop/props/prop_sound) and, for a break, its material's destroyed particles,
// since the destroy that follows removes the copy without its own break. The swinger hears it from the host too: its own call
// was cancelled, so there is nothing to predict twice. Bounded per container and in total; no late-join replay.
//
// The same lane covers every other prop (a mannequin, a crate-less barrel, a bottle): the host's component damages or
// breaks it from any cause (a fall, a throw, a bump, a swing), and a client's copy runs none of that body, so its damaged
// and destroyed particles and cues are sent too, and the receiver spawns the material's particle of that kind. A prop the
// receiver authors itself ran its own body and is skipped; a prop a client drives is that client's (its held-contact
// event, coop/player/melee_fx_contact). Game thread.

#pragma once

namespace coop::net {
class Session;
struct PropImpactFxPayload;
}  // namespace coop::net

namespace coop::props::prop_impact_fx {

// Cache the session; register the impactDamage watch once per process (it acts on a host). Called each time the install
// fanout runs.
void Install(coop::net::Session* session);

// The watch's settle. Game thread.
void Tick();

// CLIENT: the host's cue. The format was checked at the receive edge.
void OnFx(const coop::net::PropImpactFxPayload& p);

// Net disconnect: the rate state, the counts and the once-per-session latches go.
void OnDisconnect();

}  // namespace coop::props::prop_impact_fx

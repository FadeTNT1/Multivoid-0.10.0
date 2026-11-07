// ue_wrap/actors/ufo_events.h -- engine access for the gamemode's other UFO rolls (0.10.0): the boofo fly-by
// (ufo_boofo_spawn1_C, a ufo_boofo_C child), the radio tower's UFO (ufo_joel_C) and the lake glow's UFO
// (ufo_pillfo_C). Reflected reads and writes of what each one randomizes or has progressed to; the ball UFO is
// ufo_ball.h's. Engine-wrapper layer (principle 7): no gameplay, network or coop state. Game thread.
// Implementation: src/ue_wrap/actors/ufo_events.cpp.

#pragma once

#include <cstdint>

namespace ue_wrap::ufo_events {

// True if `actor` is a live instance of that exact class.
bool IsBoofo(void* actor);
bool IsJoel(void* actor);
bool IsPillfo(void* actor);

// ---- the boofo ----
//
// Its gen() lays `flyspline` as splinepoints+1 points: each is the `travelspline` point at i/splinepoints pushed out by
// `radius` along its right vector, turned by a random angle about the spline's forward axis there, and scaled by
// `squish`. The angle is everything random about it, so the setup is the angles (quantized to 256ths of a turn) and the
// progress `alpha`.
inline constexpr int kMaxBoofoPoints = 56;
struct BoofoSetup {
    float   alpha = 0.f;                 // the flight so far, 0..1
    uint8_t count = 0;                   // the points of the course (splinepoints + 1)
    uint8_t angle[kMaxBoofoPoints] = {}; // each point's angle, in 256ths of a turn
};

// Read the live course. False (out untouched) when the actor is no boofo, a member or a spline verb does not resolve, the
// course has more points than the setup holds, or `squish` is too small to recover an angle from; the reason is logged
// once. Game thread.
bool BoofoRead(void* actor, BoofoSetup& out);

// Lay a course on a boofo from its angles, the way gen() does, and set `alpha` and `length`. False when the actor is no
// boofo, the point count differs from this actor's own, or a verb does not dispatch. Game thread.
bool BoofoApply(void* actor, const BoofoSetup& in);

// ---- the joel and the pillfo: a timeline's place ----
//
// ufo_joel_c flies on its `moveufo` timeline (phase 0). ufo_pillfo_c flies on `a` (phase 0), whose finish plays
// `timeline_0` (phase 1), whose finish destroys the actor.
inline constexpr uint8_t kNoPhase = 0xFF;
struct TimelineProgress {
    uint8_t phase    = kNoPhase;   // which timeline runs now, or kNoPhase for none
    float   position = 0.f;        // seconds into it
};

// The running timeline's place. False when the actor is neither class or a timeline does not resolve. A class with no
// timeline running yields kNoPhase. Game thread.
bool TimelineRead(void* actor, TimelineProgress& out);

// The timeline `phase` of `actor`, when it is live; null otherwise (phase 1 of a joel, a dead actor, ...). Game thread.
void* TimelineOf(void* actor, uint8_t phase);

// Move a pillfo to a timeline's place: phase 1 stops `a` and plays `timeline_0` from the start before the seek. A joel's
// timeline starts a second after its BeginPlay, so its seek waits for TimelineOf(...) to be playing; this call seeks
// whatever is playing. False when the timeline is not there or a verb does not dispatch. Game thread.
bool TimelineSeek(void* actor, uint8_t phase, float position);

}  // namespace ue_wrap::ufo_events

// ue_wrap/world/sanity_ticker.h -- read-only access to the game's sanity (ticker_lightSanity_C) and a
// census of the local lights near a point. Instruments only: nothing here writes the ticker, and no
// network or co-op state lives here.
//
// The game computes sanity on every machine for its own player: the gamemode's begin-play spawns one
// ticker_lightSanity_C (mainGamemode_C.obj_tickerInsanity), whose begin-play binds playerHold to the
// gamemode's mainPlayer once. Each tick samples thingsAndStuffLib.getLightIntensityAtPoint at
// playerHold's hand (every light type, shadow-traced when the machine's shadow setting is above 0)
// into lightLevel; at or above sanityFloor the sanity rises (log2 of the excess), below it falls
// (the square of the shortfall); the gamemode's isSleep pauses it, and the scares of the insanity
// figures subtract from it. sanityFloor ramps in over the first week (newHour: day number / 7) when
// the grace-period rule is on. Nothing saves it: every world load starts it at the class default.
// Game thread only.
#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::sanity_ticker {

struct State {
    void* ticker = nullptr;
    void* playerHold = nullptr;          // the body the ticker samples
    void* gamemodePlayer = nullptr;      // the gamemode's mainPlayer now
    float sanity = 0.f;                  // 0..1, 1 = sane
    float lightLevel = 0.f, lightLevelLerp = 0.f, playerLight1 = 0.f, playerLight2 = 0.f;
    float floor = 0.f, floorOriginal = 0.f, dayMult = 0.f;
    float drain = 0.f, drainLerp = 0.f;  // per tick, before the deltaSeconds / 300 scale
    int32_t eventTier = -1;              // -1 sane, 0..4 the insanity tiers
    bool aboveFloorLerp = false;
    bool shadowCast = false;             // this machine's shadow setting (> 0) feeds the trace
    bool ruleEnable = false, drainManual = false, saveEnable = false;
    bool saveEnableRead = false;         // playerHold's SAVE_enableSanity resolved
    bool sleeping = false;               // gamemode.isSleep: the drain is paused
    bool flashlight = false;             // playerHold's flashlight is on
    bool enabled() const { return ruleEnable && drainManual && saveEnable; }
};

// The world's sanity ticker, read whole. False while the gamemode, the ticker or one of its rows
// does not resolve (the first miss of each row is logged once).
bool Read(State& out);

// One local light (point, spot or rect component) near the probe point.
struct Light {
    std::wstring owner;         // the owning actor's class
    std::wstring kind;          // the component's class
    float intensity = 0.f;
    float radius = 0.f;         // AttenuationRadius, 0 when unread
    float distance = 0.f;       // cm from the probe point
    bool visible = false;       // bVisible and not bHiddenInGame
};
struct Census {
    int scanned = 0;            // light components walked
    int inReach = 0;            // within their own attenuation radius of the point
    int litInReach = 0;         // ... and visible with an intensity above zero
    std::vector<Light> nearest; // the closest in reach, nearest first, at most `keep`
};

// Walk every live point, spot and rect light component; count those whose reach covers `at`, and
// keep the nearest `keep` of them. A GUObjectArray-index walk plus one location call per light, so
// for an instrument's slow cadence only.
bool CensusNear(const FVector& at, size_t keep, Census& out);

// A body's world location; false when it does not read.
bool LocationOf(void* actor, FVector& out);

}  // namespace ue_wrap::sanity_ticker

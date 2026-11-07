// ue_wrap/actors/event_scenes.h -- the story-event scene actors' presentation verbs: the Warning Obelisk
// (obelisk_C), Garage Soltomia's cleaner (soltomiaCleaning_C), Gray Invasion's level UFO shader
// (ufoShieldShader_C) and the anti-gravity prank's trigger (trigger_agrav_C). Each verb is what the game's own
// graph does for the player's eyes and ears at one of the scene's phases, without the graph's gameplay (the
// props it mints, the ATV it moves, the doors it jams, the player it punches). Engine-wrapper layer: no gameplay,
// network or coop state; which phase applies when is coop/world/event_scene_sync's. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::event_scenes {

// ---- reads off a script-gate call (the function and its frame) ----

// The bool parameter `param` of `function` in the frame `locals`. False when the parameter does not resolve.
bool ReadBoolParam(void* function, const uint8_t* locals, const wchar_t* param, bool& out);

// Whether the montage notify a soltomiaCleaning_C notify-begin entry carries in `locals` is its `throw`.
bool IsSoltomiaThrowNotify(void* function, const uint8_t* locals);

// ---- the cleaner (soltomiaCleaning_C) ----

// Its `active`, set by the overlap that showed it. False when the field does not resolve.
bool ReadSoltomiaActive(void* actor, bool& out);

// What its box overlap shows: the actor unhidden, the squeak and the particles started, `active` set and the
// talking timer armed (its own `talking`). The ATV move and the door jams are not touched.
bool SoltomiaReveal(void* actor);

// Its own `opened`: the half-second wait, then the cleaning montage on `solt`, whose notifies swap its face and
// play its voice lines on this machine.
bool SoltomiaPlayMontage(void* actor);

// What its throw notify shows besides the sponge: the sponge mesh in its hand hidden, the squeak and the
// particles stopped.
bool SoltomiaThrowPresentation(void* actor);

// ---- the obelisk (obelisk_C) ----

// What its landing shows besides the props and the punch: the landing sound, the landing camera shake at the
// obelisk, the falling mesh hidden (the prop the host's landing leaves takes its place) and the dust stopped.
bool ObeliskLand(void* actor);

// The running event `actor` registered for itself (lib_C::setEvent(false) with it as the sender), when it is one of
// the gamemode's senders: the obelisk's begin-play adds one that only the landing a client refuses takes back,
// and a count left above zero refuses that client's sleep.
bool EndOwnEvent(void* actor);

// Its `fallen`, written so its begin-play's one-second check takes the already-fallen branch (a joiner's mirror
// of an obelisk that has landed).
bool ObeliskMarkFallen(void* actor);

// ---- the UFO shader (ufoShieldShader_C) ----

// Its own setActive(enabled): shown or hidden, its tick and its audio with it.
bool UfoShieldSetActive(void* actor, bool enabled);

// Its own flyaway(): the exit timeline, which ends in setActive(false).
bool UfoShieldFlyAway(void* actor);

// ---- the anti-gravity trigger (trigger_agrav_C) ----

// Its own cloak(forward), the shimmer timeline, and its hover sound started or stopped with it.
bool AgravCloak(void* actor, bool forward);

// The blue wash runTrigger gives the player standing in the box and `cloak` does not: its `pointlight` attached to
// the LOCAL camera manager's root and shown (on), or destroyed as the game's own end does (off). A client whose
// player is not in the box never runs runTrigger, so it replays this beside the cloak. False when the light, the
// camera manager or a call did not resolve; a light already gone on `off` is true.
bool AgravPlayerLight(void* actor, bool on);

// The primitive components the trigger's gaher gathered (its `comps`, which the game never empties): the bodies its
// runTrigger floated and gave their gravity back. Appended to `out`; the number appended. 0 when the trigger or
// the member did not resolve.
size_t AgravGatheredComponents(void* actor, std::vector<void*>& out);

// ---- the Midas UFO (midasufotest_C, level-placed) ----

// The flight so far, `a` in 0..1 (its tick advances it from 0 to 1 along the level spline). False when the field does
// not resolve.
bool MidasProgress(void* actor, float& out);

// Its own setActive(true), the flight's start, then `a` written to `fraction` (a joiner's, mid-flight; 0 leaves the start
// alone). False when the verb did not dispatch.
bool MidasRun(void* actor, float fraction);

// ---- the eventer's walk-in saucers ----

// A ufoDropper_C child's own `forceDeploy`, the box overlap's DoOnce body: shown, its descent timeline from the
// start (a no-op once deployed). False when the verb did not dispatch.
bool UfoDropperDeploy(void* actor);

// morningUfo_C's `fly`, which its box overlap sets for a player; false when the member does not resolve.
bool ReadMorningUfoFly(void* actor, bool& out);

// What morningUfo_C's box overlap does for a player: `fly` set and the actor shown. False when either does not
// resolve.
bool MorningUfoFly(void* actor);

// ---- the vent crawler (ventCrawler_C) ----

// Its `Box`, the crawl's start: the box's world centre, its scaled half extent and its world rotation. False
// when the component or a read does not resolve.
bool ReadVentCrawlerBox(void* crawler, FVector& center, FVector& extent, FRotator& rotation);

// Its own Box begin-overlap handler, run as the engine runs it when `other` enters the box (the overlapped
// component, the other actor, no other component, body 0, no sweep). The handler is a DoOnce. False when the
// handler or the box does not resolve.
bool CallVentCrawlerOverlap(void* crawler, void* other);

// ---- level-placed actors by name ----

// The short object name of `actor` (the level's own name for it, the same on every peer that loaded the level).
std::wstring ShortName(void* actor);

// The live instance of `className` whose short name is `name` in the running world, or null. Walks the class's
// instances: for a rare event edge, never a tick.
void* FindLevelActor(const wchar_t* className, const std::wstring& name);

// The first live instance of `className` in the running world, or null. The same walk.
void* FindFirstLevelActor(const wchar_t* className);

}  // namespace ue_wrap::event_scenes

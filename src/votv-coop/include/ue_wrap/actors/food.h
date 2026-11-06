// ue_wrap/actors/food.h -- engine access for the food family (prop_food_C and every class below it, the cooking foods
// prop_cookingFood_C among them). Principle-7 engine-wrapper layer: no network or coop state.
//
// A food's own tick (prop_food_C::ReceiveTick) is the whole of its progression: it lerps `temperature` (kelvin) toward
// the room, derives `celsius`, toggles the steam component above 50 C, burns or freezes the actor at the ends of the
// range, and spends `freshnessAlpha`, taking one from `invRead_freshness` each time it runs out and calling
// `setSpoiled(true)` when none is left. A cooking food's tick adds `cookProgress` while it is hot and calls `cookItem`
// at `cookFinal`. A bite takes one from `invRead_uses`. The names are in ue_wrap/core/sdk_profile_names.h; the
// members are found on the family's base classes by name and type, never by a fixed offset.

#pragma once

#include <cstdint>

namespace ue_wrap::food {

// True iff `obj` is a live actor of prop_food_C (or a cooking food, prop_cookingFood_C) or a class below it. Game
// thread.
bool IsFood(void* obj);
bool IsCookingFood(void* obj);

// The state a food's tick advances, as the host owns it. `hasCook` says whether `cookProgress` is the actor's (a
// cooking food); `spoiled` and `frozen` are the edges the tick draws (setSpoiled, freezed).
struct State {
    float   temperature = 0.f;
    float   freshness = 0.f;
    float   cookProgress = 0.f;
    int32_t uses = 0;
    int32_t freshnessLeft = 0;
    bool    ignoreRotting = false;
    bool    spoiled = false;
    bool    frozen = false;
    bool    hasCook = false;
};

// Read the state. False, with `out` untouched, for anything that is not a food or a member that did not resolve.
bool ReadState(void* food, State& out);

// Write temperature, freshness, uses, the freshness count, ignoreRotting and (a cooking food) cookProgress as they are,
// with no verb run. The spoiled and frozen edges are RefreshDisplay's. False, writing nothing, for anything that is
// not a food or a member that did not resolve.
bool WriteState(void* food, const State& s);

// What the tick shows of the state, drawn without the tick: `celsius` from the temperature and the steam component on
// above 50 C; on a spoiled edge (`was` against `now`) the game's own setSpoiled; on a frozen edge `tempLessThan0`,
// the game's own freezed and the mesh's custom primitive data. True when the celsius and the steam were drawn.
bool RefreshDisplay(void* food, const State& now, const State& was);

}  // namespace ue_wrap::food

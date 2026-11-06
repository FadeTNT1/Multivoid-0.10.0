// ue_wrap/actors/breakable_explosive.h -- the props whose own break blasts: the vending machine, the gas can, the
// bombs, the explosive crystal and the explosive drive (profile::name::BreakableExplosiveClasses). Their damage runs
// through the prop's physicsImpact component like any prop's, and its break calls the owner's `broken`, which spawns
// an explosion_C and, for the vending machine, spills half its stock as fresh props and lights the spill.
// Engine-wrapper layer: which class is one, nothing about who may break it.
#pragma once

namespace ue_wrap::breakable_explosive {

// True when `actor`'s class is one of the listed classes or below. A class's verdict is kept, and asked again once
// the object index has unlisted a class (a kept class pointer may then name a freed one). Game thread.
bool IsBreakableExplosive(void* actor);

// True when `actor` is a vending machine (prop_vending_C or below). Game thread.
bool IsVending(void* actor);

}  // namespace ue_wrap::breakable_explosive

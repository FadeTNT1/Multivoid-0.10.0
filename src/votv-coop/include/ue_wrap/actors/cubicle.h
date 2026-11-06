// ue_wrap/actors/cubicle.h -- the office cubicle partitions: the class prop_cubicle_C and plain prop_C partitions of
// list_props row cubicleP_<n> (profile::name::CubicleClass, CubicleItemRowBase). Their damage runs through the prop's
// physicsImpact component, whose break plays the material's destroyed sound and particles, may spawn gibs, and
// destroys the owner. Engine-wrapper layer: which actor is one, nothing about who may break it.
#pragma once

namespace ue_wrap::cubicle {

// Resolve the row base's name. Dispatches the string-to-name conversion, so call it from a top-level game-thread
// pass, never from inside a hook. Idempotent; true once resolved. Until then IsCubicle matches the class alone.
bool ResolveNames();

// True when `actor`'s class is prop_cubicle_C or below, or `actor` is a prop whose row is cubicleP_<n>. Game thread.
bool IsCubicle(void* actor);

}  // namespace ue_wrap::cubicle

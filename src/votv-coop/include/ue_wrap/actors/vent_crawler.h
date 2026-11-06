// ue_wrap/actors/vent_crawler.h -- the ventCrawler_C event actor's link to the wall vent it pulls down.
// Principle-7 engine-wrapper layer: no network or coop state.
//
// The ventCrawler event binds its `vent` variable to the prop_vent_C keyed `ventHanging` in
// ReceiveBeginPlay, animates that actor with the ventbreakTL timeline (K2_SetActorTransform each
// frame), then runs setPropProps(static false, frozen true, active false, sleep false) on it when
// the timeline finishes. This reads the variable, by name and type, so a caller can follow the actor
// the game itself is moving.

#pragma once

namespace ue_wrap::vent_crawler {

// The prop_vent_C that `crawler` (a live ventCrawler_C) holds in its `vent` variable. Null for a null
// or dead crawler, a class without an ObjectProperty of that name (logged once), an unbound variable,
// or a value that is not a live prop_vent_C. Game thread.
void* VentOf(void* crawler);

}  // namespace ue_wrap::vent_crawler

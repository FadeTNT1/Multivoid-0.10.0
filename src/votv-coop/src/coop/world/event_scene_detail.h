// coop/world/event_scene_detail.h -- INTERNAL (src-tree) header shared by the event-scene module's translation
// units: event_scene_sync.cpp (the phase table, the wire, a client's apply), event_scene_refuse.cpp (the watches:
// the host's phase edges and a client's refusals), event_scene_agrav.cpp (the prank's floating props near the
// clients), event_scene_crawler.cpp (the box scenes' walk-in by a client) and event_scene_tower.cpp (the radio
// tower's ship). Nothing outside these includes it.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::event_scene_sync::detail {

// The module's session (null when none).
coop::net::Session* Session();
bool IsHost();
bool IsActiveClient();

// HOST: scene `scene` reached `phase` on `actor` (a world actor of this lane, or a level-placed actor). Recorded as
// the scene's newest phase and sent once its element is known. Game thread.
void NoteHostPhase(uint8_t scene, uint8_t phase, void* actor);

// The watches, registered once per process (event_scene_refuse.cpp).
void InstallWatches();
// The prank's floating props near the clients (event_scene_agrav.cpp): its cloak edge on the host, and the
// teardown.
void AgravHostEdge(void* trigger, bool on);
// CLIENT: the host's cloak phase arrived (on at the start, off at the end).
void AgravClientEdge(bool on);
void AgravOnDisconnect();
// The vent crawler's walk-in by a client's player, and the sound relay's arming while a crawler lives
// (event_scene_crawler.cpp). HOST, each tick (it paces itself).
void CrawlerHostTick();
void CrawlerOnDisconnect();
// Whether `p` lies in the box at `c` with half extent `e` turned by `r`, padded by `pad` (event_scene_crawler.cpp).
bool InsideBox(const ue_wrap::FVector& p, const ue_wrap::FVector& c, const ue_wrap::FVector& e,
               const ue_wrap::FRotator& r, float pad);
// The radio tower's ariral ship (event_scene_tower.cpp). HOST, each tick (it paces itself): a client's walk-in is
// rolled here the way the ship's own loop rolls the host's. CLIENT: the host's phase played on the level copy, the
// one call of `appear` a client lets run; ArirTowerRefuseOwn is the watch's verdict for every other call.
void ArirTowerHostTick();
bool ArirTowerClientApply(void* actor, bool shown);
bool ArirTowerApplying();
void ArirTowerOnDisconnect();

}  // namespace coop::event_scene_sync::detail

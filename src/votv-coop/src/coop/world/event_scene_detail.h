// coop/world/event_scene_detail.h -- INTERNAL (src-tree) header shared by the event-scene module's translation
// units: event_scene_sync.cpp (the phase table, the wire, a client's apply), event_scene_refuse.cpp (the watches:
// the host's phase edges and a client's refusals) and event_scene_agrav.cpp (the prank's floating props near the
// clients). Nothing outside these three includes it.

#pragma once

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

}  // namespace coop::event_scene_sync::detail

// coop/world/event_scene_sync.h -- the story-event scenes the host runs, shown on every client: the Warning
// Obelisk, Garage Soltomia, Hole Repair, the end of Gray Invasion and the anti-gravity prank. Each scene's
// gameplay (its props, punch, ATV move, door jams, patrol, tank AI, floating bodies) stays the host's and crosses
// on the lanes that carry those. What a player SEES crosses here: the scenes' actors are mirrored (npc_world_enum's
// source rows) with their gameplay bodies refused on a client (event_scene_refuse); the host's watches see a scene
// reach a phase and send EventScenePhase, which a client plays on its own copy (ue_wrap::event_scenes) once.
// Players: the obelisk's punch of player 0 reaches every client's own body (player_hit), and the prank floats the
// props near every client too (event_scene_agrav). Late join: each live scene's newest phase after its spawns, as
// an end state; the obelisk, the hole's room and the fire tanks come from the joiner's own save instead.
// Host authoritative; game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventScenePhasePayload;
}  // namespace coop::net

namespace coop::event_scene_sync {

// Cache the session and register the watches (each once per process, live as its class loads). Called every pump
// tick by the subsystems install. Game thread.
void Install(coop::net::Session* session);

// HOST: send the phases whose world actor has its element now, drop the scenes whose actor died. CLIENT: retry the
// level-placed phases whose actor was not found yet. Game thread.
void Tick();

// HOST, at a joiner's world-ready edge, after its world-actor spawns: the newest phase of every live scene.
void QueueConnectBroadcastForSlot(int slot);

// Format check of a received phase (the scene, the phase, the flags, the target). Any thread.
bool WellFormed(const coop::net::EventScenePhasePayload& p);

// CLIENT: a host phase (event_feed, game thread).
void OnPhase(const coop::net::EventScenePhasePayload& p);

// CLIENT: the world-actor mirror `eid` has just been materialised as `actor`: a phase that arrived before it lands
// now. Game thread.
void OnMirrorMaterialized(uint32_t eid, void* actor);

// Teardown: every scene and held phase dropped.
void OnDisconnect();

}  // namespace coop::event_scene_sync

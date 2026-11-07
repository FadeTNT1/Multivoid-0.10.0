// coop/creatures/host_born_spawn.h -- the births the host authors whatever Blueprint made them.
//
// A Blueprint mints an actor through BeginDeferredActorSpawnFromClass under EX_CallMath, a route the ProcessEvent
// interceptors of the npc and world-actor lanes never see, so a creature born that way crossed only when its
// spawner had a source row in coop/creatures/npc_world_enum. This table names classes instead: on the host each
// birth of a row's class (or a class below it) is caught by that lane's Func-thunk catch and mirrored, and on a
// client the same birth made by a Blueprint is dissolved (its result nulled, the unfinished actor destroyed by a
// posted task), the explosion lane's shape. The gamemode is never a producer here: its save load, its dreams and
// its rolls keep their own rows. A client's own mirror spawns go through ProcessEvent and are never dissolved. A
// row may name the only producers it covers (the stolas volume's lightning), and such a row is client-only: its
// class reaches a client through its own lane. Game thread, inside the engine's spawn: cheap, allocation-free.

#pragma once

namespace coop::host_born_spawn {

// Resolve the rows' class names to engine names, once; until then no birth is caught or dissolved. Called every
// pump tick by npc_sync's install, outside any native callback (the conversion dispatches ProcessEvent). Game thread.
void ResolveNames();

// Host: is `spawned`, minted by `sourceObject`'s bytecode, a birth the host mirrors whatever made it? Game thread.
bool HostCatches(void* sourceObject, void* spawned);

// Client: dissolve `spawned` when a Blueprint of `sourceObject` minted a row's class during an active client
// session. Called from the same catch for every deferred spawn; a no-op on the host and in single player. Game thread.
void OnClientBirth(void* sourceObject, void* spawned);

}  // namespace coop::host_born_spawn

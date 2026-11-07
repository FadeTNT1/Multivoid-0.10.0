// coop/items/save_actor_sync.h -- the persistent actors a held tool leaves behind are the host's: a sticker, a candle,
// a string of lights, a weld, a nail, a taped door, a rug, a snowman, a built wall, a jumper cable. Each is an
// actor_save_C, not a prop_C, so no prop lane carries it; ue_wrap/actors/save_actor names the allowlist. One lane for all
// of them, keyed by the actor's own save key and carried as the actor's own save record (docs/deployables.md).
//
// CATCH. A post on GameplayStatics::FinishSpawningActor (chained with the other consumers of that seam) sees each birth
// of an allowlisted class, and an end-of-play sink sees each death. A birth the gamemode makes (a save load) is the
// save's and never crosses; neither does one this lane makes itself.
//
// HOST. Every other birth is the host's: a tick after it (the Blueprint fills the actor's state after the finish -- a
// sticker attaches there) the key is minted through the game's own getKey and SaveActorState carries the birth, its
// owner and the record to every client. A death of an allowlisted actor with a key goes out by key, and is kept as a
// tombstone for a joiner. A known mutator (a light string's addLight and finish, a weld's weld, a candle's ignite, a
// rug's setSkin, a jumper cable's connect) republishes the record when it changed.
//
// CLIENT. A birth crosses only when a player authored it here (prop_spawn_authoring: a player verb on the stack, or a
// source born under one -- the nail a fired projectile mints in its own tick); the client keeps its copy, marks it out
// of the save, and asks the host as SaveActorIntent under a request id with the key and the record. The host spawns the
// canonical one with the client's key (a new key when that one is taken), applies the record and processKeys, and its
// broadcast doubles as the answer: the client binds its own copy on the echo, renamed when the host renamed it; a
// refusal retires it. A class whose processKeys ties two props with a physics constraint (the nail) is the exception
// to keeping the copy: the client's goes once its record is sent and the host's mirror takes its place, and no mirror
// runs processKeys, so the constraint exists on the host alone. A birth whose source is an allowlisted actor (the wall
// scaffold turning into the wall) is the host's step to take: the client's own is retired at once. A client's own
// death of an allowlisted actor is a destroy intent, unless the class retires itself on every peer. A mutator on a
// client is a state intent. A mirror is spawned out of the save, its key written in the deferred window, its record
// applied through loadData and its attach keys resolved through processKeys.
//
// LATE JOIN. The host's own copies are in its save. At a joiner's ready edge the host sends every birth this session
// made and every key destroyed since; the joiner adopts a save-loaded copy by key instead of spawning a second.
// Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct BlobChunkPayload;
}  // namespace coop::net

namespace coop::save_actor_sync {

// Cache the session; install the spawn seam, the end-of-play sink and the mutator watches once. Re-entered by the
// install pump.
void Install(coop::net::Session* session);

// The watches' settle, the class refresh; the births and deaths the seams caught; HOST: the queued intents, the
// mutators' records; CLIENT: the parked states; the outbound queue.
void Tick(coop::net::Session& session);

// HOST: a joiner's world is up: every key destroyed since the session began, then every birth this session made.
void OnPeerWorldReady(int slot);

// HOST: a client's birth, state or destroy. CLIENT: the host's.
void OnIntentChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
void OnStateChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: a client left: its half-assembled blobs and its queued intents go. Its actors stay: the host owns them.
void OnPeerLeft(uint8_t slot);

// Net disconnect: the tables, queues and counters go, with one summary line. Every actor stays where it is.
void OnDisconnect();

}  // namespace coop::save_actor_sync

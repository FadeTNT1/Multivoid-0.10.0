// coop/items/inventory_wire.h -- serialize the per-player profile <-> a byte blob.
//
// Turns coop::player_profile::Profile (the inventory component's own bytes, the vitals and the
// pose) into a self-contained, version-prefixed little-endian byte blob, and back. The items are
// opaque here: they are the game's own serialization of the player's inventory component, which
// names classes and objects by path, so they carry across machines running the same build.
//
// The blob is what coop/blob_chunks carries in chunks (client->host), what persists to
// <mod data dir>/coop_players/<hostSlot>/<guid>.json, and what the join apply reads back.

#pragma once

#include "coop/items/player_profile.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::inventory_wire {

// Current blob format version (the first byte). Bump on any change of layout OR meaning. Versions
// 1 to 3 held 0.9.0n's save records (saveSlot.GObjStack[0], equipment, hold), which 0.10.0 no
// longer has; they do not parse, and the host quarantines a stored one.
//   4 -- a present byte and the inventory component's bytes, then the vitals and the pose.
inline constexpr uint8_t kVersion = 4;

// The largest inventory a blob may carry: the transport ceiling (blob_chunks::MaxBlobBytes(), about
// 21.4 MB; player_inventory_sync.cpp static_asserts this stays above it). A lower bound here would refuse a profile the transport delivers, and the host would
// take the refusal for a corrupt file.
inline constexpr uint32_t kMaxItemBytes = 25'000'000u;

// Serialize `p` into a fresh blob.
std::vector<uint8_t> Serialize(const coop::player_profile::Profile& p);

// The same blob in its two halves, for a caller that polls: the items change when the player
// acts and the vitals every second, so the items half is built and hashed on its own, and the
// state half is appended only when the blob is going to be sent.
std::vector<uint8_t> SerializeItems(bool hasItems, const std::vector<uint8_t>& items);
void AppendState(std::vector<uint8_t>& blob, const ue_wrap::vitals::Snapshot* vitals,
                 const coop::player_profile::Pose& pose);  // null vitals = none were read

// Parse `blob` back into `out` (cleared first). False on a truncated / malformed / unknown-
// version blob (a corrupt or hostile blob must never over-read or over-allocate).
bool Deserialize(const std::vector<uint8_t>& blob, coop::player_profile::Profile& out);

}  // namespace coop::inventory_wire

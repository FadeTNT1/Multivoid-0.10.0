// coop/items/poster_sync_detail.h -- sibling-internal surface shared by the poster lane's two TUs (poster_sync.cpp,
// the host, the watches and the wire; poster_mirror.cpp, a client's placed posters). Private to the src tree, never
// part of the include surface.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <string>

namespace coop::poster_sync::detail {

// The wire's file name: "None" for none; false when the name does not fit 95 bytes or is not well-formed UTF-8.
bool FileNameToWire(const std::wstring& name, coop::net::PosterFileName& out);
bool FileNameFromWire(const coop::net::PosterFileName& in, std::wstring& out);

// A key the wire can carry.
bool KeyFits(const std::wstring& key);

// A custom image this peer lacks was drawn as its preset: counted, said once.
void SayFallback(const std::wstring& key, const std::wstring& file);

// CLIENT: land the host's state (placed, removed, the snapshot's end); retry the states parked for a class not
// loaded yet; the counts; the session-end reset. Game thread.
void MirrorState(const coop::net::PosterStatePayload& p);
void MirrorTick();
struct MirrorCounts {
    uint64_t applied = 0, spawned = 0, destroyed = 0, dropped = 0;
};
MirrorCounts Counts();
void MirrorReset();

}  // namespace coop::poster_sync::detail

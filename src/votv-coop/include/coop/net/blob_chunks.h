// coop/net/blob_chunks.h -- chunked variable-length blob transport over the reliable lane,
// shared by EVERY chunked-row kind on the wire -- a dozen of them, from the e-mail and saved-signal
// appends through the drive, rack, laptop, floppy-box, container, meadow-DB and
// per-player-inventory payloads. Two semantics callers rely on:
//
//   ALL-OR-NOTHING -- SendBlob returns true only if EVERY chunk was accepted. The caller
//                     retries the whole blob next poll under a FRESH seq, and the receiver's
//                     dangling half-assembly is TTL-swept.
//   RESTART        -- a mismatched part (a rejoining sender reusing (slot, seq) against a
//                     stale half-assembly) erases the stale entry and restarts from the
//                     incoming chunk when that chunk is a stream start (chunkIdx == 0). Only
//                     a true mid-stream orphan drops.
//
// A blob of more than blob_pacer.h kBatchMaxChunks chunks is not handed over in one call: the owner
// feeds it through PacedSender (the per-player inventory is the one that can be that large).
//
// Also home to the FNV-1a 64 content hash the delete mirrors key on. Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace coop::net { class Session; }

namespace coop::blob_chunks {

// FNV-1a 64 -- the cross-peer content identity of a serialized row.
inline uint64_t Fnv64(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}
inline uint64_t Fnv64(const std::vector<uint8_t>& b) { return Fnv64(b.data(), b.size()); }

// The most chunks one blob may span. The receiver refuses a larger `chunks`, the sender refuses a
// larger blob. The wire count is a u32, so this is policy, not a field limit.
inline constexpr uint32_t kMaxBlobChunks = 100000;

// The transport ceiling: kMaxBlobChunks x the inline chunk payload (about 21.4 MB). A blob past this
// NEVER sends (ChunkAndSend warns and returns false before any chunk), so a caller with unbounded
// content must bound BELOW this and check the return -- an ignored false on a canonical path is a
// silent permanent divergence. A blob this size also has to fit the send path (see
// SendBacklog::SendOrQueueBatch): in practice the sender's backlog byte cap lowers the deliverable
// size to roughly 13 MB.
inline constexpr size_t MaxBlobBytes() {
    return sizeof(coop::net::BlobChunkPayload{}.data) * kMaxBlobChunks;
}

// The ceiling every lane had before the chunk count widened (255 chunks of 220 bytes). The lanes
// that truncate or refuse to fit a blob size against THIS, not MaxBlobBytes(), so their behaviour
// and the data one peer can make another buffer stay as they were. Only the per-player inventory
// (a profile that may never be truncated) uses the full MaxBlobBytes().
inline constexpr size_t kLaneBlobBytes = 56100;

// Ship one blob as BlobChunkPayload chunks under `kind` with per-sender id `seq`. True only if
// every chunk was accepted.
bool SendBlob(coop::net::Session* s, coop::net::ReliableKind kind, uint32_t seq,
              const std::vector<uint8_t>& blob);

// Slot-targeted variant: ship the blob's chunks to ONE peer (peerSlot) via
// Session::SendReliableToSlot instead of the default route. Used by the HOST to send a specific
// client its per-player inventory. Same all-or-nothing semantics as SendBlob: true only if every
// chunk was accepted by that slot.
bool SendBlobToSlot(coop::net::Session* s, int peerSlot, coop::net::ReliableKind kind,
                    uint32_t seq, const std::vector<uint8_t>& blob);

// Per-kind reassembler; each sync module owns one (keys are per-sender, and
// kinds ride separate Assembler instances, so seq spaces never collide).
class Assembler {
public:
    // Feed one chunk (validates the part fields). Returns true exactly when a
    // blob completed; the assembled bytes are moved into outBlob.
    bool OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot,
                 std::vector<uint8_t>& outBlob);

    // TTL-sweep half-assemblies that have taken no chunk for `ttl` (call ~1 Hz).
    void Sweep(std::chrono::steady_clock::time_point now, std::chrono::seconds ttl);

    void Clear();

    // Chunks taken so far of the newest half-assembly from `senderSlot`, with the total it expects;
    // false when none is held. A waiter's progress signal (a paced blob is slow, not dead).
    bool Progress(uint8_t senderSlot, uint32_t& got, uint32_t& expect) const;

    // A counter that rises with every chunk this assembler accepts: a waiter that sees it move knows
    // the sender is alive. Never reset by Clear().
    uint64_t ChunksTaken() const { return chunksTaken_; }

    // Whether no half-assembly is held: a consumer sweeps only while one is.
    bool Idle() const { return map_.empty(); }

    // Drop every half-assembly from ONE sender slot. Call from the per-slot disconnect fan-out: a
    // recycled slot (X -> Y, with no observable absence between them) must not merge Y's chunks
    // into X's partial.
    void ClearSlot(uint8_t senderSlot);

private:
    struct Assembly {
        uint32_t expectChunks = 0, gotChunks = 0;
        std::vector<uint8_t> blob;
        // The time of the LAST chunk taken (set on the first too): the TTL measures silence, so a
        // blob that is paced out over many seconds is not swept while it is still arriving.
        std::chrono::steady_clock::time_point lastChunk{};
    };
    std::map<std::pair<uint8_t, uint32_t>, Assembly> map_;
    uint64_t chunksTaken_ = 0;
};

}  // namespace coop::blob_chunks

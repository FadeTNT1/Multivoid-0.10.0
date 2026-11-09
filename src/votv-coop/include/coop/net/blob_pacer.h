// coop/net/blob_pacer.h -- paced delivery of a blob too large to hand the transport in one call.
//
// SendBlob / SendBlobToSlot build every chunk and give the whole run to the backlog in one call,
// which is right up to kBatchMaxChunks (255): the run fits the send buffer, and no join gate may
// split it. A bigger blob (a per-player inventory can be megabytes) would fill the buffer, then the
// backlog, and past the backlog's byte cap the connection would be kicked. So a blob past
// kBatchMaxChunks is held HERE and fed a bounded number of chunks per tick through
// Session::TrySendReliableToSlot, the direct attempt the save stream paces itself with: it refuses
// under send-buffer backpressure and under the paced-stream time bound (send_admission.h), and a
// refused chunk is retried next tick from the SAME index. The blob is never restarted and never
// truncated.
//
// Per peer slot: one job in flight and at most one newest-wins pending, so a periodic producer (the
// client's profile stream) cannot pile up. Game thread only (Tick, Enqueue and the cancels).
//
// A job dies only three ways, and each is visible to the owner: CancelSlot (that slot's disconnect),
// CancelAll (session stop), or a stall of kStallLimit with the slot still connected, which Tick
// reports in `abandoned` so the owner can re-enqueue under a fresh seq.

#pragma once

#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"  // kMaxPeers

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace coop::net { class Session; }

namespace coop::blob_chunks {

// The most chunks the one-call batch path takes; a blob of more is paced.
inline constexpr uint32_t kBatchMaxChunks = 255;

// Chunks one tick may hand over per slot. The paced-stream time bound is the real throttle.
inline constexpr int kPacedChunksPerTick = 64;

// A job that moved no chunk for this long, with its slot still connected, is abandoned.
inline constexpr auto kStallLimit = std::chrono::seconds(30);

// How many chunks a blob of `bytes` spans (an empty blob is one chunk).
uint32_t ChunkCount(size_t bytes);

class PacedSender {
public:
    // Take `blob` for `peerSlot` (0 = the host, from a client). False only when the blob is past
    // MaxBlobBytes() (nothing is queued). A job already moving keeps going and this one waits as the
    // newest pending; one that has not sent a chunk yet is replaced.
    bool Enqueue(int peerSlot, coop::net::ReliableKind kind, uint32_t seq, std::vector<uint8_t> blob);

    // Feed every slot's job. Slots whose job was abandoned by the stall bound are appended to
    // `abandoned`.
    void Tick(coop::net::Session* s, std::vector<int>& abandoned);

    // Whether a job is in flight or pending for the slot.
    bool Busy(int peerSlot) const;

    // Drop the slot's jobs (its disconnect: a recycled slot must never receive them) / every job.
    void CancelSlot(int peerSlot);
    void CancelAll();

    // Chunks handed over so far / total for the slot's job in flight; false when none.
    bool Progress(int peerSlot, uint32_t& sent, uint32_t& total) const;

private:
    struct Job {
        coop::net::ReliableKind kind{};
        uint32_t seq = 0;
        std::vector<uint8_t> blob;
        uint32_t chunks = 0;
        uint32_t next = 0;
        std::chrono::steady_clock::time_point lastProgress{};
    };
    struct Slot {
        std::optional<Job> cur;
        std::optional<Job> pending;
    };
    Slot slots_[coop::players::kMaxPeers];
};

}  // namespace coop::blob_chunks

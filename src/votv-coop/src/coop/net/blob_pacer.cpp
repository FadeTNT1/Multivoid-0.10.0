// coop/net/blob_pacer.cpp -- see coop/net/blob_pacer.h.

#include "coop/net/blob_pacer.h"

#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::blob_chunks {

uint32_t ChunkCount(size_t bytes) {
    const size_t maxData = sizeof(coop::net::BlobChunkPayload{}.data);
    const size_t n = bytes == 0 ? 1 : (bytes + maxData - 1) / maxData;
    return n > kMaxBlobChunks ? kMaxBlobChunks + 1 : static_cast<uint32_t>(n);
}

bool PacedSender::Enqueue(int peerSlot, coop::net::ReliableKind kind, uint32_t seq,
                          std::vector<uint8_t> blob) {
    if (peerSlot < 0 || peerSlot >= coop::players::kMaxPeers) return false;
    const uint32_t chunks = ChunkCount(blob.size());
    if (chunks > kMaxBlobChunks) {
        UE_LOGW("blob_pacer: blob of %zu bytes exceeds the %u-chunk cap (kind %u) -- not queued",
                blob.size(), kMaxBlobChunks, static_cast<unsigned>(kind));
        return false;
    }
    Job job;
    job.kind = kind;
    job.seq = seq;
    job.blob = std::move(blob);
    job.chunks = chunks;
    job.lastProgress = std::chrono::steady_clock::now();
    Slot& sl = slots_[peerSlot];
    if (sl.cur && sl.cur->next > 0) sl.pending = std::move(job);  // the one moving finishes first
    else { sl.cur = std::move(job); sl.pending.reset(); }
    return true;
}

void PacedSender::Tick(coop::net::Session* s, std::vector<int>& abandoned) {
    if (!s) return;
    const auto now = std::chrono::steady_clock::now();
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        Slot& sl = slots_[slot];
        if (!sl.cur && sl.pending) { sl.cur = std::move(sl.pending); sl.pending.reset(); }
        if (!sl.cur) continue;
        if (!s->IsSlotConnected(slot)) {  // the disconnect fan-out cancels too; this is the backstop
            sl = Slot{};
            continue;
        }
        Job& j = *sl.cur;
        const size_t maxData = sizeof(coop::net::BlobChunkPayload{}.data);
        for (int n = 0; n < kPacedChunksPerTick && j.next < j.chunks; ++n) {
            coop::net::BlobChunkPayload p{};
            p.blobSeq = j.seq;
            p.chunkIdx = j.next;
            p.chunks = j.chunks;
            const size_t off = static_cast<size_t>(j.next) * maxData;
            const size_t take = j.blob.size() - off > maxData ? maxData : j.blob.size() - off;
            p.chunkLen = static_cast<uint16_t>(take);
            if (take) std::memcpy(p.data, j.blob.data() + off, take);
            // A refusal (backpressure, the paced-stream time bound, the pre-world gate) is the
            // pacing signal: stop for this tick and retry the SAME chunk next tick.
            if (!s->TrySendReliableToSlot(slot, j.kind, &p, static_cast<int>(sizeof(p)))) break;
            ++j.next;
            j.lastProgress = now;
        }
        if (j.next >= j.chunks) {
            UE_LOGI("blob_pacer: slot %d kind %u seq=%u -- all %u chunks handed to the transport",
                    slot, static_cast<unsigned>(j.kind), j.seq, j.chunks);
            sl.cur.reset();
            continue;
        }
        if (now - j.lastProgress > kStallLimit) {
            UE_LOGE("blob_pacer: slot %d kind %u seq=%u stalled at chunk %u of %u for %lld s -- "
                    "abandoned, the owner re-sends under a fresh seq", slot,
                    static_cast<unsigned>(j.kind), j.seq, j.next, j.chunks,
                    static_cast<long long>(kStallLimit.count()));
            sl = Slot{};
            abandoned.push_back(slot);
        }
    }
}

bool PacedSender::Busy(int peerSlot) const {
    if (peerSlot < 0 || peerSlot >= coop::players::kMaxPeers) return false;
    return slots_[peerSlot].cur.has_value() || slots_[peerSlot].pending.has_value();
}

void PacedSender::CancelSlot(int peerSlot) {
    if (peerSlot < 0 || peerSlot >= coop::players::kMaxPeers) return;
    slots_[peerSlot] = Slot{};
}

void PacedSender::CancelAll() {
    for (Slot& sl : slots_) sl = Slot{};
}

bool PacedSender::Progress(int peerSlot, uint32_t& sent, uint32_t& total) const {
    if (peerSlot < 0 || peerSlot >= coop::players::kMaxPeers || !slots_[peerSlot].cur) return false;
    sent = slots_[peerSlot].cur->next;
    total = slots_[peerSlot].cur->chunks;
    return true;
}

}  // namespace coop::blob_chunks

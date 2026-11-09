// coop/net/blob_chunks.cpp -- see coop/net/blob_chunks.h.

#include "coop/net/blob_chunks.h"

#include "coop/net/session.h"

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::blob_chunks {

// W4 anti-flood: concurrent half-assemblies allowed from ONE sender, per Assembler instance.
// Real use is one blob at a time per lane (the sender chunks a blob and finishes it before
// starting the next), so 4 is generous; the TTL sweep reclaims anything abandoned.
constexpr size_t kMaxAssembliesPerSender = 4;

// The most a half-assembly reserves up front, in chunks (about 55 KB). `chunks` is ATTACKER-CHOSEN
// and may be up to kMaxBlobChunks, so the reserve is a small fixed bound and the vector grows only
// by the chunks the sender really delivers: one 228-byte packet buys at most this much memory.
constexpr uint32_t kReserveChunks = 256;

namespace {
size_t ReserveBytes(uint32_t chunks) {
    return static_cast<size_t>(chunks < kReserveChunks ? chunks : kReserveChunks) *
           sizeof(coop::net::BlobChunkPayload{}.data);
}

// Shared chunker for SendBlob / SendBlobToSlot. It builds every chunk first, then `sendAll(chunks,
// n)` hands the whole blob to the transport in one call (broadcast vs slot-targeted), so a join gate
// cannot split it; everything else (chunk count, cap, all-or-nothing) is identical (RULE 2 --
// one chunking implementation). The chunk buffer is reused across blobs.
template <class SendAll>
bool ChunkAndSend(coop::net::ReliableKind kind, uint32_t seq,
                  const std::vector<uint8_t>& blob, SendAll sendAll) {
    static thread_local std::vector<coop::net::BlobChunkPayload> chunkBuf;
    const size_t maxData = sizeof(coop::net::BlobChunkPayload{}.data);
    // The count is computed as size_t and checked against the cap BEFORE it is narrowed.
    const size_t wanted = blob.empty() ? 1 : (blob.size() + maxData - 1) / maxData;
    if (wanted > kMaxBlobChunks) {
        UE_LOGW("blob_chunks: blob of %zu bytes exceeds the %u-chunk cap (kind %u) -- dropped",
                blob.size(), kMaxBlobChunks, static_cast<unsigned>(kind));
        return false;
    }
    const uint32_t chunks = static_cast<uint32_t>(wanted);
    chunkBuf.assign(chunks, coop::net::BlobChunkPayload{});
    size_t off = 0;
    for (uint32_t i = 0; i < chunks; ++i) {
        coop::net::BlobChunkPayload& p = chunkBuf[i];
        p.blobSeq = seq;
        p.chunkIdx = i;
        p.chunks = chunks;
        const size_t take = blob.size() - off > maxData ? maxData : blob.size() - off;
        p.chunkLen = static_cast<uint16_t>(take);
        if (take) std::memcpy(p.data, blob.data() + off, take);
        off += take;
    }
    const bool sent = sendAll(chunkBuf.data(), static_cast<int>(chunks));
    // An unusually large blob must not pin its chunk buffer for the life of the thread.
    if (chunkBuf.capacity() > 4 * kReserveChunks) std::vector<coop::net::BlobChunkPayload>().swap(chunkBuf);
    if (!sent) {
        UE_LOGW("blob_chunks: send of %u chunks refused (kind %u, seq=%u) -- "
                "blob will retry under a fresh seq",
                chunks, static_cast<unsigned>(kind), seq);
        return false;
    }
    return true;
}
}  // namespace

bool SendBlob(coop::net::Session* s, coop::net::ReliableKind kind, uint32_t seq,
              const std::vector<uint8_t>& blob) {
    return ChunkAndSend(kind, seq, blob,
                        [&](const coop::net::BlobChunkPayload* chunks, int n) {
                            return s->SendReliableBatch(kind, chunks, sizeof(*chunks), n);
                        });
}

bool SendBlobToSlot(coop::net::Session* s, int peerSlot, coop::net::ReliableKind kind,
                    uint32_t seq, const std::vector<uint8_t>& blob) {
    return ChunkAndSend(kind, seq, blob,
                        [&](const coop::net::BlobChunkPayload* chunks, int n) {
                            return s->SendReliableBatchToSlot(peerSlot, kind, chunks,
                                                              sizeof(*chunks), n);
                        });
}

bool Assembler::OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot,
                        std::vector<uint8_t>& outBlob) {
    if (p.chunks == 0 || p.chunks > kMaxBlobChunks || p.chunkIdx >= p.chunks ||
        p.chunkLen > sizeof(p.data))
        return false;

    const auto now = std::chrono::steady_clock::now();
    const auto key = std::make_pair(senderSlot, p.blobSeq);
    // Anti-flood caps. blobSeq is ATTACKER-CHOSEN, and `map_[key]` below default-inserts, so without
    // them one 228-byte packet bought a fresh assembly. Two bounds, both PER-SENDER (a global table
    // would let one flooding peer starve every other peer's assemblies):
    //  - the NUMBER of assemblies (kMaxAssembliesPerSender), checked when a new key arrives;
    //  - the TOTAL bytes a sender's assemblies hold (MaxBlobBytes()): `chunks` may name 21 MB, and
    //    the vector grows only by chunks really sent, but 4 such assemblies would still be 85 MB
    //    per lane. Holding at most one maximal blob's worth per sender bounds that.
    // The up-front reserve is a small fixed bound (ReserveBytes), never chunks * data.
    // order_sync caps its own table the same way -- this is that guard at the shared primitive, so
    // every lane gets it at once instead of eight bespoke patches.
    const bool known = map_.find(key) != map_.end();
    size_t fromSender = 0, bytesFromSender = 0;
    for (const auto& kv : map_) {
        if (kv.first.first != senderSlot) continue;
        ++fromSender;
        bytesFromSender += kv.second.blob.size();
    }
    if (!known && fromSender >= kMaxAssembliesPerSender) {
        UE_LOGW("blob_chunks: sender %u has %zu concurrent assemblies (cap %zu) -- dropping new "
                "blobSeq=%u", static_cast<unsigned>(senderSlot), fromSender,
                kMaxAssembliesPerSender, p.blobSeq);
        return false;
    }
    if (bytesFromSender + p.chunkLen > MaxBlobBytes()) {
        UE_LOGW("blob_chunks: sender %u holds %zu B in half-assemblies (cap %zu) -- dropping "
                "blobSeq=%u and its partial", static_cast<unsigned>(senderSlot), bytesFromSender,
                MaxBlobBytes(), p.blobSeq);
        map_.erase(key);
        return false;
    }
    auto& a = map_[key];
    if (a.expectChunks == 0) {
        a.expectChunks = p.chunks;
        a.lastChunk = now;
        a.blob.reserve(ReserveBytes(p.chunks));
    }
    if (a.expectChunks != p.chunks || a.gotChunks != p.chunkIdx) {
        // Mismatched part: the RESTART semantics (see header).
        map_.erase(key);
        if (p.chunkIdx != 0) return false;
        auto& fresh = map_[key];
        fresh.expectChunks = p.chunks;
        fresh.lastChunk = now;
        fresh.blob.reserve(ReserveBytes(p.chunks));
        fresh.blob.insert(fresh.blob.end(), p.data, p.data + p.chunkLen);
        fresh.gotChunks = 1;
        ++chunksTaken_;
        if (fresh.gotChunks == fresh.expectChunks) {
            outBlob = std::move(fresh.blob);
            map_.erase(key);
            return true;
        }
        return false;
    }
    a.blob.insert(a.blob.end(), p.data, p.data + p.chunkLen);
    ++a.gotChunks;
    a.lastChunk = now;
    ++chunksTaken_;
    if (a.gotChunks < a.expectChunks) return false;

    outBlob = std::move(a.blob);
    map_.erase(key);
    return true;
}

bool Assembler::Progress(uint8_t senderSlot, uint32_t& got, uint32_t& expect) const {
    const Assembly* best = nullptr;
    for (const auto& kv : map_) {
        if (kv.first.first != senderSlot) continue;
        if (!best || kv.second.lastChunk > best->lastChunk) best = &kv.second;
    }
    if (!best) return false;
    got = best->gotChunks;
    expect = best->expectChunks;
    return true;
}

void Assembler::Sweep(std::chrono::steady_clock::time_point now, std::chrono::seconds ttl) {
    for (auto it = map_.begin(); it != map_.end();) {
        if (now - it->second.lastChunk > ttl) it = map_.erase(it);
        else ++it;
    }
}

void Assembler::Clear() { map_.clear(); }

void Assembler::ClearSlot(uint8_t senderSlot) {
    // Slot teardown is a ROW TRANSITION: slots recycle lowest-free with no observable absence, so
    // person Y's chunks must never merge into person X's half-assembly under the same slot id. The
    // 20 s TTL alone leaves that window open, so callers hook this into the per-slot disconnect
    // fan-out.
    for (auto it = map_.begin(); it != map_.end();) {
        if (it->first.first == senderSlot) it = map_.erase(it);
        else ++it;
    }
}

}  // namespace coop::blob_chunks

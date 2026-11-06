// coop/props/storage_act_host.cpp -- the host's half of storage_act: a client's transfer judged on the host's own
// copy, applied through the storage's verbs, held until the client's commit, undone on an abort, a lapse or the
// client's departure. See coop/props/storage_act.h.
//
// A take's row comes off the storage at the grant and rides the reply; an abort, a lapsed hold or the taker's
// departure puts it back. A put's row goes in at the grant; an abort takes it out again, and a lapse or a departure
// leaves it in, since the client may already have given it up: a row is never lost to a lost commit, at worst it is
// in two places.

#include "coop/props/storage_act_internal.h"

#include "coop/element/intent_authority.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/props/container_reach.h"
#include "coop/props/storage_contents.h"
#include "coop/props/storage_wire.h"
#include "ue_wrap/actors/inventory_item_codec.h"
#include "ue_wrap/actors/prop_gather.h"   // VolumeOfName
#include "ue_wrap/actors/storage_inventory.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace coop::props::storage_act::host {
namespace {

namespace SI = ue_wrap::storage_inventory;
namespace W = coop::props::storage_wire;
namespace SC = coop::props::storage_contents;
using Clock = std::chrono::steady_clock;
using coop::net::ContainerActReason;

constexpr auto kHoldBound = std::chrono::seconds(30);
constexpr size_t kHoldsPerSlot = 4;
constexpr size_t kAnsweredKept = 256;

std::atomic<coop::net::Session*> g_session{nullptr};

struct Hold {
    uint8_t slot = 0;
    uint16_t txn = 0;
    W::Family family = W::Family::Take;
    W::Address address;
    std::vector<uint8_t> row;      // the row as it went out (a take) or came in (a put)
    uint64_t fingerprint = 0;
    uint8_t amount = 0;
    Clock::time_point deadline{};
};
std::vector<Hold> g_holds;

// The replies already sent, by (slot, txn): a repeat of a request is answered with the same bytes, never run again.
std::map<std::pair<uint8_t, uint16_t>, std::vector<uint8_t>> g_answered;
std::deque<std::pair<uint8_t, uint16_t>> g_answeredOrder;
coop::blob_chunks::Assembler g_requestAsm, g_commitAsm;
uint32_t g_replySeq = 1;
uint32_t g_malformed = 0;

coop::net::Session* Session() { return g_session.load(std::memory_order_acquire); }
bool IsHost() { auto* s = Session(); return s && s->role() == coop::net::Role::Host; }

const char* FamilyName(W::Family f) { return f == W::Family::Take ? "take" : "put"; }

W::Reply Refuse(uint16_t txn, ContainerActReason why) {
    W::Reply r;
    r.txn = txn;
    r.reason = static_cast<uint8_t>(why);
    return r;
}

// The sender reaches the storage: the container prop through the lane's own reach (an actor that opens it counts),
// the drone at the drone.
ContainerActReason JudgeReach(coop::net::Session& s, uint8_t slot, const W::Address& a, void* component) {
    if (a.kind == W::AddressKind::Prop) {
        switch (coop::props::container_reach::Judge(s, slot, a.id).reach) {
            case coop::props::container_reach::Reach::Reached: return ContainerActReason::Granted;
            case coop::props::container_reach::Reach::NoBody: return ContainerActReason::Lapsed;
            case coop::props::container_reach::Reach::Unreachable: return ContainerActReason::Reach;
        }
    }
    const auto tok = coop::element::IntentTarget::ForClientIntent(s, slot, coop::props::container_reach::kReachUU);
    switch (tok.Authorize(SI::OwnerOf(component)).outcome) {
        case coop::element::IntentOutcome::Ok: return ContainerActReason::Granted;
        case coop::element::IntentOutcome::NoBody: return ContainerActReason::Lapsed;
        default: return ContainerActReason::Reach;
    }
}

// The row an add stored: the entry of `after` that `before` does not hold (matched by item and place, not by ordinal,
// since the add may take a free slot ahead of other rows). False when the add stacked onto a row already there.
bool StoredRow(const std::vector<SI::RowPlace>& before, const std::vector<SI::RowPlace>& after, SI::RowPlace& out) {
    std::vector<bool> used(before.size(), false);
    for (const SI::RowPlace& a : after) {
        bool matched = false;
        for (size_t i = 0; i < before.size(); ++i) {
            const SI::RowPlace& b = before[i];
            if (used[i] || b.fingerprint != a.fingerprint || b.x != a.x || b.y != a.y || b.rotation != a.rotation)
                continue;
            used[i] = matched = true;
            break;
        }
        if (!matched) {
            out = a;
            return true;
        }
    }
    return false;
}

W::Reply Judge(uint8_t slot, const W::Request& q) {
    coop::net::Session* const s = Session();
    if (!s->IsSlotWorldReady(slot)) return Refuse(q.txn, ContainerActReason::Lapsed);
    if (q.plan != ue_wrap::inventory_item_codec::PlanSignature()) return Refuse(q.txn, ContainerActReason::Refused);
    void* const comp = SC::Resolve(q.address);
    if (!comp) return Refuse(q.txn, ContainerActReason::Gone);
    if (const ContainerActReason r = JudgeReach(*s, slot, q.address, comp); r != ContainerActReason::Granted)
        return Refuse(q.txn, r);
    size_t held = 0;
    for (const Hold& h : g_holds) held += h.slot == slot ? 1 : 0;
    if (held >= kHoldsPerSlot) return Refuse(q.txn, ContainerActReason::QueueFull);
    SI::Terms terms;
    if (!SI::ReadTerms(comp, terms)) return Refuse(q.txn, ContainerActReason::Refused);
    if (terms.locked) return Refuse(q.txn, ContainerActReason::Locked);

    Hold h;
    h.slot = slot;
    h.txn = q.txn;
    h.family = q.family;
    h.address = q.address;
    h.fingerprint = q.fingerprint;
    h.amount = q.amount;
    h.deadline = Clock::now() + kHoldBound;
    W::Reply reply;
    reply.txn = q.txn;
    if (q.family == W::Family::Take) {
        const int32_t index = SI::FindRow(comp, q.fingerprint, q.amount);
        uint64_t fp = 0;
        uint8_t rowAmount = 0;
        if (index < 0 || !SI::RowInfo(comp, index, fp, rowAmount)) return Refuse(q.txn, ContainerActReason::Gone);
        if (!SI::EncodeRowAt(comp, index, h.row, q.amount)) return Refuse(q.txn, ContainerActReason::Refused);
        const bool off = q.amount == rowAmount ? SI::RemoveRowAt(comp, index) : SI::ReduceRowAt(comp, index, q.amount);
        if (!off) return Refuse(q.txn, ContainerActReason::Refused);
        reply.row = h.row;
    } else {
        std::wstring name;
        uint8_t amount = 0;
        uint64_t fp = 0;
        if (!SI::PeekRow(q.row, name, amount, fp) || fp != q.fingerprint || amount != q.amount)
            return Refuse(q.txn, ContainerActReason::Refused);
        if (!terms.infinite) {
            float volume = 0.f;
            bool valid = false;
            if (!ue_wrap::prop_gather::VolumeOfName(name, volume, valid) || !valid)
                return Refuse(q.txn, ContainerActReason::Refused);
            if (terms.totalVolume + volume * static_cast<float>(amount) > terms.maxVolume + 1e-3f)
                return Refuse(q.txn, ContainerActReason::NoSpace);
        }
        std::vector<SI::RowPlace> placedBefore, placedAfter;
        const bool readBefore = SI::ReadPlacements(comp, placedBefore);
        if (!SI::AddRow(comp, q.row, q.keepPlace)) return Refuse(q.txn, ContainerActReason::NoSpace);
        // The line names the place the STORED row carries, read back after the add, beside the place that was asked for.
        float x = 0.f, y = 0.f, rot = 0.f;
        const bool asked = SI::PeekPlacement(q.row, x, y, rot);
        SI::RowPlace stored;
        if (readBefore && SI::ReadPlacements(comp, placedAfter) && StoredRow(placedBefore, placedAfter, stored))
            UE_LOGI("storage_act[host]: slot %u put txn %u placement applied: stored x=%.1f y=%.1f rot=%.1f "
                    "(asked x=%.1f y=%.1f rot=%.1f, %s)",
                    static_cast<unsigned>(slot), q.txn, stored.x, stored.y, stored.rotation, x, y, rot,
                    q.keepPlace ? "the drop's own place" : "a random place");
        else
            UE_LOGI("storage_act[host]: slot %u put txn %u placement applied: no new row to read back (stacked onto a "
                    "row there; asked x=%.1f y=%.1f rot=%.1f, %s)",
                    static_cast<unsigned>(slot), q.txn, asked ? x : 0.f, asked ? y : 0.f, asked ? rot : 0.f,
                    q.keepPlace ? "the drop's own place" : "a random place");
        h.row = q.row;
    }
    SC::MarkDirty(comp);
    reply.granted = true;
    reply.revision = SC::RevisionOf(q.address);
    UE_LOGI("storage_act[host]: slot %u %s txn %u at %s x%u granted (%zu B row)", static_cast<unsigned>(slot),
            FamilyName(q.family), q.txn, SC::Describe(q.address), static_cast<unsigned>(q.amount), h.row.size());
    g_holds.push_back(std::move(h));
    return reply;
}

// The host's half undone: a take's row back in, a put's row out again.
void Undo(const Hold& h, const char* why) {
    void* const comp = SC::Resolve(h.address);
    bool ok = false;
    if (comp && h.family == W::Family::Take) {
        ok = SI::AddRow(comp, h.row, /*keepPlace=*/true) || SI::AddRow(comp, h.row, /*keepPlace=*/false);
    } else if (comp) {
        const int32_t index = SI::FindRow(comp, h.fingerprint, h.amount);
        uint64_t fp = 0;
        uint8_t rowAmount = 0;
        if (index >= 0 && SI::RowInfo(comp, index, fp, rowAmount))
            ok = rowAmount == h.amount ? SI::RemoveRowAt(comp, index) : SI::ReduceRowAt(comp, index, h.amount);
    }
    if (comp) SC::MarkDirty(comp);
    if (ok)
        UE_LOGI("storage_act[host]: slot %u %s txn %u undone (%s)", static_cast<unsigned>(h.slot),
                FamilyName(h.family), h.txn, why);
    else
        UE_LOGW("storage_act[host]: slot %u %s txn %u could not be undone (%s): %s", static_cast<unsigned>(h.slot),
                FamilyName(h.family), h.txn, why,
                !comp ? "the storage is gone" : h.family == W::Family::Take ? "the storage refused the row back"
                                                                             : "the row was taken meanwhile");
}

// A hold that ends with no commit: a take is put back, a put stays where it went.
void Lapse(const Hold& h, const char* why) {
    if (h.family == W::Family::Take) {
        Undo(h, why);
        return;
    }
    UE_LOGI("storage_act[host]: slot %u put txn %u kept with no commit (%s)", static_cast<unsigned>(h.slot), h.txn,
            why);
}

void Remember(uint8_t slot, uint16_t txn, std::vector<uint8_t> reply) {
    const auto key = std::make_pair(slot, txn);
    if (g_answered.emplace(key, std::move(reply)).second) g_answeredOrder.push_back(key);
    while (g_answeredOrder.size() > kAnsweredKept) {
        g_answered.erase(g_answeredOrder.front());
        g_answeredOrder.pop_front();
    }
}

bool SendReply(uint8_t slot, const std::vector<uint8_t>& blob) {
    return coop::blob_chunks::SendBlobToSlot(Session(), slot, coop::net::ReliableKind::ContainerActReply,
                                             g_replySeq++, blob);
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    if (!IsHost() || g_holds.empty()) return;
    const auto now = Clock::now();
    for (auto it = g_holds.begin(); it != g_holds.end();) {
        if (now < it->deadline) { ++it; continue; }
        const Hold h = std::move(*it);
        it = g_holds.erase(it);
        Lapse(h, "no commit in time");
    }
    if (!g_requestAsm.Idle()) g_requestAsm.Sweep(now, std::chrono::seconds(30));
    if (!g_commitAsm.Idle()) g_commitAsm.Sweep(now, std::chrono::seconds(30));
}

void OnDisconnect() {
    std::vector<Hold> holds;
    holds.swap(g_holds);
    for (const Hold& h : holds) Lapse(h, "the session ended");   // only the host ever holds one
    g_answered.clear();
    g_answeredOrder.clear();
    g_requestAsm.Clear();
    g_commitAsm.Clear();
    g_malformed = 0;
}

void OnPeerGone(uint8_t slot) {
    if (!IsHost()) return;
    for (auto it = g_holds.begin(); it != g_holds.end();) {
        if (it->slot != slot) { ++it; continue; }
        const Hold h = std::move(*it);
        it = g_holds.erase(it);
        Lapse(h, "the client left");
    }
    for (auto it = g_answeredOrder.begin(); it != g_answeredOrder.end();) {
        if (it->first != slot) { ++it; continue; }
        g_answered.erase(*it);
        it = g_answeredOrder.erase(it);
    }
    g_requestAsm.ClearSlot(slot);
    g_commitAsm.ClearSlot(slot);
}

void OnRequestChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!IsHost()) return;
    std::vector<uint8_t> blob;
    if (!g_requestAsm.OnChunk(p, senderSlot, blob)) return;
    W::Request q;
    if (!W::ParseRequest(blob, q)) {
        if (++g_malformed <= 3 || g_malformed % 50 == 0)
            UE_LOGW("storage_act[host]: a malformed request from slot %u (%zu B) dropped (#%u)",
                    static_cast<unsigned>(senderSlot), blob.size(), g_malformed);
        return;
    }
    if (q.family == W::Family::Layout) {
        OnLayoutRequest(senderSlot, q);   // no answer, no hold: the host publishes its state either way
        return;
    }
    if (const auto it = g_answered.find({senderSlot, q.txn}); it != g_answered.end()) {
        SendReply(senderSlot, it->second);   // a repeat: the same answer, nothing run again
        return;
    }
    const W::Reply reply = Judge(senderSlot, q);
    if (!reply.granted)
        UE_LOGI("storage_act[host]: slot %u %s txn %u at %s refused (reason %u)", static_cast<unsigned>(senderSlot),
                FamilyName(q.family), q.txn, SC::Describe(q.address), static_cast<unsigned>(reply.reason));
    std::vector<uint8_t> out = W::PackReply(reply);
    if (!SendReply(senderSlot, out) && reply.granted) {
        // The grant cannot reach its client: its half is never made, so the host's is undone now.
        for (auto it = g_holds.begin(); it != g_holds.end(); ++it)
            if (it->slot == senderSlot && it->txn == q.txn) {
                const Hold h = std::move(*it);
                g_holds.erase(it);
                Undo(h, "the grant could not be sent");
                break;
            }
        return;
    }
    Remember(senderSlot, q.txn, std::move(out));
}

void OnCommitChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!IsHost()) return;
    std::vector<uint8_t> blob;
    if (!g_commitAsm.OnChunk(p, senderSlot, blob)) return;
    W::Commit c;
    if (!W::ParseCommit(blob, c)) return;
    for (auto it = g_holds.begin(); it != g_holds.end(); ++it) {
        if (it->slot != senderSlot || it->txn != c.txn) continue;
        const Hold h = std::move(*it);
        g_holds.erase(it);
        if (c.committed)
            UE_LOGI("storage_act[host]: slot %u %s txn %u committed", static_cast<unsigned>(senderSlot),
                    FamilyName(h.family), h.txn);
        else
            Undo(h, "the client aborted");
        return;
    }
    UE_LOGI("storage_act[host]: slot %u commit for txn %u matches no hold (late or repeated)",
            static_cast<unsigned>(senderSlot), c.txn);
}

void OnLayoutRequest(uint8_t slot, const W::Request& q) {
    coop::net::Session* const s = Session();
    if (!s || !IsHost()) return;
    const char* const cause = q.cause == W::LayoutCause::Sort ? "sort" : "settle";
    if (!s->IsSlotWorldReady(slot) || q.plan != ue_wrap::inventory_item_codec::PlanSignature()) {
        UE_LOGI("storage_act[host]: slot %u %s layout ignored (%s)", static_cast<unsigned>(slot), cause,
                !s->IsSlotWorldReady(slot) ? "its world is not ready" : "its row plan is not this peer's");
        return;
    }
    void* const comp = SC::Resolve(q.address);
    if (!comp) {
        UE_LOGI("storage_act[host]: slot %u %s layout at %s refused (the storage is gone)",
                static_cast<unsigned>(slot), cause, SC::Describe(q.address));
        return;
    }
    // Refused from here on, the host's own state goes out again so the sender's local layout is redrawn from it.
    const auto refuse = [&](const char* why) {
        UE_LOGI("storage_act[host]: slot %u %s layout at %s refused (%s) -- rev %u, the host's state published again",
                static_cast<unsigned>(slot), cause, SC::Describe(q.address), why, SC::RevisionOf(q.address));
        SC::Resync(q.address);
    };
    if (const ContainerActReason r = JudgeReach(*s, slot, q.address, comp); r != ContainerActReason::Granted) {
        refuse(r == ContainerActReason::Reach ? "out of reach" : "no body");
        return;
    }
    SI::Terms terms;
    if (!SI::ReadTerms(comp, terms) || terms.locked) {
        refuse("locked");
        return;
    }
    if (q.revision > SC::RevisionOf(q.address)) {
        refuse("a revision the host has not published");
        return;
    }
    // The revision the client saw may be behind the host's by a placement-only publish; what must hold is the items:
    // the same row count and, at each named ordinal, the same item. ApplyLayout checks both before it touches a row.
    std::vector<SI::RowPlace> places;
    places.reserve(q.layout.size());
    for (const W::LayoutEntry& e : q.layout) {
        SI::RowPlace p;
        p.ordinal = e.ordinal;
        p.fingerprint = e.fingerprint;
        p.x = e.x;
        p.y = e.y;
        p.rotation = e.rotation;
        places.push_back(p);
    }
    const SI::LayoutResult r = SI::ApplyLayout(comp, q.rowCount, places);
    if (r != SI::LayoutResult::Applied) {
        refuse(r == SI::LayoutResult::Stale ? "the items changed since the client's revision"
                                            : "the rows did not take it");
        return;
    }
    SC::MarkDirty(comp);
    // Read the stored rows back: the line reports what the host now holds, not what was asked.
    std::vector<SI::RowPlace> stored;
    size_t differ = 0;
    if (SI::ReadPlacements(comp, stored))
        for (const SI::RowPlace& p : places)
            if (p.ordinal < stored.size() && (stored[p.ordinal].x != p.x || stored[p.ordinal].y != p.y ||
                                              stored[p.ordinal].rotation != p.rotation))
                ++differ;
    UE_LOGI("storage_act[host]: slot %u %s layout applied rows=%zu of %u rev=%u (read back: %zu differ)",
            static_cast<unsigned>(slot), cause, places.size(), static_cast<unsigned>(q.rowCount),
            SC::RevisionOf(q.address), differ);
}

}  // namespace coop::props::storage_act::host

// coop/props/storage_act_client.cpp -- the client's half of storage_act: the gates on a world storage's verbs, the
// intent, the host's answer and the commit. The host's half is storage_act_host.cpp. See coop/props/storage_act.h.

#include "coop/props/storage_act.h"
#include "coop/props/storage_act_internal.h"

#include "coop/comms/chat_feed.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/storage_contents.h"
#include "coop/props/storage_wire.h"
#include "l10n/l10n.h"
#include "ue_wrap/actors/inventory_component.h"
#include "ue_wrap/actors/inventory_item_codec.h"
#include "ue_wrap/actors/storage_inventory.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <string>
#include <vector>

namespace coop::props::storage_act {
namespace {

namespace SG = ue_wrap::script_gate;
namespace SI = ue_wrap::storage_inventory;
namespace W = coop::props::storage_wire;
namespace SC = coop::props::storage_contents;
using Clock = std::chrono::steady_clock;
using coop::net::ContainerActReason;

UE_FACT_FN(kRowBeginDrag, "storage_act", L"ui_freeLayoutInventory_C", L"beginDrag", Declared);
UE_FACT_FN(kRowAddAdvanced, "storage_act", L"inventoryComponent_C", L"addItemAdvanced", Declared);
UE_FACT_FN(kRowRemove, "storage_act", L"inventoryComponent_C", L"removeInventoryItem", Declared);
UE_FACT_FN(kRowTakeFromStack, "storage_act", L"uicomp_FLI_item_C", L"takeFromStack", Declared);
UE_FACT_FN(kRowAddToStack, "storage_act", L"uicomp_FLI_item_C", L"addToStack", Declared);
UE_FACT_FN(kRowDoubleClick, "storage_act", L"ui_freeLayoutInventory_C", L"doubleClickEvent", Declared);
constexpr int kTagGate = 0x53544131;   // 'STA1'

// An honest answer comes in milliseconds; past this the act is forgotten and a late grant is answered with an abort.
constexpr auto kReplyBound = std::chrono::seconds(10);
constexpr auto kLineEvery = std::chrono::seconds(2);

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_gatesRegistered = false;

struct Pending {
    bool open = false;
    uint16_t txn = 0;
    W::Family family = W::Family::Take;
    W::Address address;
    uint64_t fingerprint = 0;
    uint8_t amount = 0;
    Clock::time_point sent{};
};
Pending g_pending;
uint16_t g_nextTxn = 1;
uint32_t g_requestSeq = 1, g_commitSeq = 1;
std::deque<std::vector<uint8_t>> g_unsentCommits;
coop::blob_chunks::Assembler g_replyAsm;
Clock::time_point g_lastLine{};
uint32_t g_refusedWrites = 0;

coop::net::Session* Session() { return g_session.load(std::memory_order_acquire); }
bool IsClient() { auto* s = Session(); return s && s->role() == coop::net::Role::Client; }

void Line(const char* text) {
    const auto now = Clock::now();
    if (now - g_lastLine < kLineEvery) return;
    g_lastLine = now;
    coop::chat_feed::Push(std::string(text), coop::chat_feed::Keep::Transient);
}

void LineFor(uint8_t reason) {
    switch (static_cast<ContainerActReason>(reason)) {
        case ContainerActReason::Gone:      Line(l10n::T("That item is gone.")); break;
        case ContainerActReason::Reach:     Line(l10n::T("That container is too far away.")); break;
        case ContainerActReason::NoSpace:   Line(l10n::T("Not enough space.")); break;
        case ContainerActReason::Locked:    Line(l10n::T("That container is locked.")); break;
        case ContainerActReason::Refused:   Line(l10n::T("That can't go in there.")); break;
        case ContainerActReason::QueueFull: Line(l10n::T("Too many actions at once, try again.")); break;
        default:                            Line(l10n::T("That can't be done right now.")); break;
    }
}

const char* FamilyName(W::Family f) { return f == W::Family::Take ? "take" : "put"; }

// A write the client may not make, refused; said in the log now and then, never per call.
SG::Verdict RefuseWrite(const char* verb) {
    if (++g_refusedWrites <= 3 || g_refusedWrites % 50 == 0)
        UE_LOGI("storage_act[client]: %s on a world storage refused (#%u) -- the host's copy decides", verb,
                g_refusedWrites);
    return SG::Verdict::Cancel;
}

bool Guarded(void* component) {
    return IsClient() && !SC::InApply() && SI::IsWorldStorage(component);
}

void SendCommit(uint16_t txn, bool committed) {
    std::vector<uint8_t> blob = W::PackCommit(W::Commit{txn, committed});
    if (!coop::blob_chunks::SendBlob(Session(), coop::net::ReliableKind::ContainerActCommit, g_commitSeq++, blob))
        g_unsentCommits.push_back(std::move(blob));
}

void Ask(W::Request r) {
    if (g_pending.open) {
        LineFor(static_cast<uint8_t>(ContainerActReason::QueueFull));
        return;
    }
    r.txn = g_nextTxn++;
    if (g_nextTxn == 0) g_nextTxn = 1;
    r.plan = ue_wrap::inventory_item_codec::PlanSignature();
    r.revision = SC::RevisionOf(r.address);
    const std::vector<uint8_t> blob = W::PackRequest(r);
    if (blob.empty()) {
        LineFor(static_cast<uint8_t>(ContainerActReason::Refused));
        return;
    }
    if (!coop::blob_chunks::SendBlob(Session(), coop::net::ReliableKind::ContainerActRequest, g_requestSeq++, blob)) {
        Line(l10n::T("The host could not be asked; try again."));
        return;
    }
    g_pending = Pending{true, r.txn, r.family, r.address, r.fingerprint, r.amount, Clock::now()};
    UE_LOGI("storage_act[client]: asked %s txn %u at %s x%u (rev %u, %zu B)", FamilyName(r.family), r.txn,
            SC::Describe(r.address), static_cast<unsigned>(r.amount), r.revision, blob.size());
    float x = 0.f, y = 0.f, rot = 0.f;
    if (r.family == W::Family::Put && SI::PeekPlacement(r.row, x, y, rot))
        UE_LOGI("storage_act[client]: put txn %u placement sent x=%.1f y=%.1f rot=%.1f (%s)", r.txn, x, y, rot,
                r.keepPlace ? "the drop's own place" : "a random place");
}

// beginDrag of a row a world storage holds: the take starts here, so it is asked of the host instead.
SG::Verdict OnBeginDrag(const SG::Call& c) {
    if (!IsClient()) return SG::Verdict::Run;
    void* comp = nullptr;
    int32_t index = -1;
    if (!SI::BeginDragRow(c.locals, c.function, comp, index) || !Guarded(comp)) return SG::Verdict::Run;
    W::Request r;
    uint8_t rowAmount = 0;
    if (!SC::AddressOf(comp, r.address) || !SI::RowInfo(comp, index, r.fingerprint, rowAmount)) {
        LineFor(static_cast<uint8_t>(ContainerActReason::Lapsed));
        return SG::Verdict::Cancel;
    }
    r.family = W::Family::Take;
    r.amount = SI::BeginDragAmount(rowAmount);
    Ask(std::move(r));
    return SG::Verdict::Cancel;
}

// An add into a world storage: from the inventory screen it is a put asked of the host; from anywhere else (a hand
// deposit, a loot fill) it is refused, and the item stays where it was.
SG::Verdict OnAddAdvanced(const SG::Call& c) {
    if (!Guarded(c.object)) return SG::Verdict::Run;
    if (!SI::IsInventoryScreen(c.callerObject)) {
        LineFor(static_cast<uint8_t>(ContainerActReason::Lapsed));
        return RefuseWrite("addItemAdvanced (not the inventory screen's)");
    }
    W::Request r;
    r.family = W::Family::Put;
    if (!SC::AddressOf(c.object, r.address) ||
        !SI::AddCallRow(c.locals, c.function, r.row, r.fingerprint, r.amount, r.keepPlace)) {
        LineFor(static_cast<uint8_t>(ContainerActReason::Refused));
        return SG::Verdict::Cancel;
    }
    Ask(std::move(r));
    return SG::Verdict::Cancel;
}

SG::Verdict OnRemove(const SG::Call& c) {
    return Guarded(c.object) ? RefuseWrite("removeInventoryItem") : SG::Verdict::Run;
}

SG::Verdict OnStack(const SG::Call& c) {
    return Guarded(SI::ItemWidgetComponent(c.object)) ? RefuseWrite("a stack split or merge") : SG::Verdict::Run;
}

// A double-click uses the row it lands on (a spawned copy of the item runs its own action, then the row is removed): on
// a world storage's row the action would run here and the removal be refused.
SG::Verdict OnDoubleClick(const SG::Call& c) {
    return Guarded(SI::DoubleClickComponent(c.object)) ? RefuseWrite("a double-click on a stored row")
                                                       : SG::Verdict::Run;
}

void RegisterGates() {
    if (g_gatesRegistered) return;
    const bool a = SG::WatchName(kRowBeginDrag, kTagGate, &OnBeginDrag, nullptr);
    const bool b = SG::WatchName(kRowAddAdvanced, kTagGate, &OnAddAdvanced, nullptr);
    const bool c = SG::WatchName(kRowRemove, kTagGate, &OnRemove, nullptr);
    const bool d = SG::WatchName(kRowTakeFromStack, kTagGate, &OnStack, nullptr);
    const bool e = SG::WatchName(kRowAddToStack, kTagGate, &OnStack, nullptr);
    const bool f = SG::WatchName(kRowDoubleClick, kTagGate, &OnDoubleClick, nullptr);
    g_gatesRegistered = a && b && c && d && e && f;
    if (g_gatesRegistered) SG::ResolvePendingNames();
    else UE_LOGE("storage_act: a storage gate was refused (beginDrag %d addItemAdvanced %d removeInventoryItem %d "
                 "takeFromStack %d addToStack %d doubleClickEvent %d) -- a client's storage writes are not held for the "
                 "host", a, b, c, d, e, f);
}

// The client's half of a granted act: the row into the player's inventory (a take, `widget` the item widget drawn for
// it) or out of it (a put).
bool RunGranted(const W::Reply& reply, void*& widget) {
    widget = nullptr;
    void* const pawn = coop::players::Registry::Get().Local();
    void* const mine = pawn ? ue_wrap::inventory_component::Of(pawn) : nullptr;
    if (!mine) return false;
    if (g_pending.family == W::Family::Take) return !reply.row.empty() && SI::GiveToPlayer(mine, reply.row, widget);
    return SI::TakeFromPlayer(mine, g_pending.fingerprint, g_pending.amount);
}

// A granted take's item is in the player's inventory; the drag the player began on it (cancelled until the host
// answered) goes on from there, as the game's own beginDrag leaves it.
void ResumeHeldDrag(uint16_t txn, void* widget) {
    const SI::HeldDrag how = SI::ResumeDrag(widget);
    switch (how) {
        case SI::HeldDrag::Held:
            UE_LOGI("storage_act[client]: take txn %u granted -> drag resumed, the item is held", txn);
            break;
        case SI::HeldDrag::Dropped:
            UE_LOGI("storage_act[client]: take txn %u granted -> drag resumed, the button was up: ended at the cursor",
                    txn);
            break;
        case SI::HeldDrag::Stored:
            UE_LOGI("storage_act[client]: take txn %u granted -> fallback, the item is in the inventory (%s)", txn,
                    widget ? "the screen could not hold it" : "no item widget drawn");
            break;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterGates();
    host::Install(session);
}

void Tick() {
    host::Tick();
    if (!IsClient()) return;
    if (g_pending.open && Clock::now() - g_pending.sent > kReplyBound) {
        UE_LOGW("storage_act[client]: %s txn %u got no answer in %llds -- forgotten", FamilyName(g_pending.family),
                g_pending.txn, static_cast<long long>(kReplyBound.count()));
        g_pending = Pending{};
    }
    while (!g_unsentCommits.empty() &&
           coop::blob_chunks::SendBlob(Session(), coop::net::ReliableKind::ContainerActCommit, g_commitSeq++,
                                       g_unsentCommits.front()))
        g_unsentCommits.pop_front();
    if (!g_replyAsm.Idle()) g_replyAsm.Sweep(Clock::now(), std::chrono::seconds(30));
}

void OnDisconnect() {
    host::OnDisconnect();
    g_pending = Pending{};
    g_unsentCommits.clear();
    g_replyAsm.Clear();
    g_refusedWrites = 0;
}

void OnPeerGone(uint8_t slot) { host::OnPeerGone(slot); }

void OnRequestChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) { host::OnRequestChunk(p, senderSlot); }

void OnCommitChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) { host::OnCommitChunk(p, senderSlot); }

void OnReplyChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!IsClient() || senderSlot != 0) return;
    std::vector<uint8_t> blob;
    if (!g_replyAsm.OnChunk(p, senderSlot, blob)) return;
    W::Reply reply;
    if (!W::ParseReply(blob, reply)) {
        UE_LOGW("storage_act[client]: a malformed reply (%zu B) dropped", blob.size());
        return;
    }
    if (!g_pending.open || reply.txn != g_pending.txn) {
        // A grant for an act this client gave up on: the host undoes its half.
        if (reply.granted) SendCommit(reply.txn, false);
        UE_LOGI("storage_act[client]: txn %u answered after it was forgotten (%s)", reply.txn,
                reply.granted ? "granted, aborted" : "refused");
        return;
    }
    if (!reply.granted) {
        UE_LOGI("storage_act[client]: %s txn %u refused (reason %u)", FamilyName(g_pending.family), reply.txn,
                static_cast<unsigned>(reply.reason));
        LineFor(reply.reason);
        g_pending = Pending{};
        return;
    }
    void* widget = nullptr;
    const bool done = RunGranted(reply, widget);
    SendCommit(reply.txn, done);
    UE_LOGI("storage_act[client]: %s txn %u granted at %s x%u -> %s", FamilyName(g_pending.family), reply.txn,
            SC::Describe(g_pending.address), static_cast<unsigned>(g_pending.amount),
            done ? "done" : "aborted (the player's inventory refused its half)");
    if (!done) LineFor(static_cast<uint8_t>(ContainerActReason::NoSpace));
    const bool take = g_pending.family == W::Family::Take;
    g_pending = Pending{};
    // After the act is closed: a drag that ends at once may start the next one (a put into a storage).
    if (done && take) ResumeHeldDrag(reply.txn, widget);
}

SG::Verdict GateTransfer(const SG::Call& c) {
    if (!IsClient() || SC::InApply()) return SG::Verdict::Run;
    if (SI::IsWorldStorage(c.object) || SI::IsWorldStorage(SI::TransferCallTarget(c.locals, c.function)))
        return RefuseWrite("transferFromInvToInf");
    return SG::Verdict::Run;
}

}  // namespace coop::props::storage_act

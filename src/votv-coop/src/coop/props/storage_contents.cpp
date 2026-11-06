// coop/props/storage_contents.cpp -- see coop/props/storage_contents.h.

#include "coop/props/storage_contents.h"
#include "coop/props/storage_act.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "ue_wrap/actors/inventory_item_codec.h"
#include "ue_wrap/actors/storage_inventory.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

namespace coop::props::storage_contents {
namespace {

namespace SG = ue_wrap::script_gate;
namespace SI = ue_wrap::storage_inventory;
namespace IC = ue_wrap::inventory_item_codec;
namespace W = coop::props::storage_wire;
namespace R = ue_wrap::reflection;
using Clock = std::chrono::steady_clock;

UE_FACT_FN(kRowCalculateVolumes, "storage_contents", L"inventoryComponent_C", L"calculateVolumes", Declared);
UE_FACT_FN(kRowTransfer, "storage_contents", L"inventoryComponent_C", L"transferFromInvToInf", Declared);
UE_FACT_FN(kRowDespawn, "storage_contents", L"uicomp_FLI_canvas_C", L"despawnCanvas", Declared);
constexpr int kTagCalculate = 0x53544331;   // 'STC1'
constexpr int kTagDespawn   = 0x53544333;   // 'STC3'
constexpr int kTagTransfer  = 0x53544332;   // 'STC2'

constexpr auto kDrainEvery = std::chrono::milliseconds(250);
constexpr auto kDronePollEvery = std::chrono::seconds(1);
constexpr auto kWaitBound = std::chrono::seconds(30);

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchesRegistered = false;

// Host: the storages marked since the last drain, and per storage the revision and the hash of the rows last sent.
std::set<W::Address> g_dirty;
struct Sent { uint32_t revision = 0; uint64_t hash = 0; bool known = false; };
std::map<W::Address, Sent> g_sent;
Clock::time_point g_nextDrain{}, g_nextDronePoll{};
uint32_t g_nextSeq = 1;

// Client: the revision last applied per storage, the pages of a set being assembled, the sets waiting for an actor.
std::map<W::Address, uint32_t> g_applied;
struct Assembly { uint32_t revision = 0; uint8_t pages = 0; uint8_t next = 0; std::vector<std::vector<uint8_t>> rows; };
std::map<W::Address, Assembly> g_assembling;
struct Waiting { uint32_t revision = 0; std::vector<std::vector<uint8_t>> rows; Clock::time_point since; };
std::map<W::Address, Waiting> g_waiting;
coop::blob_chunks::Assembler g_asm;
bool g_inApply = false;

coop::net::Session* Session() { return g_session.load(std::memory_order_acquire); }
bool IsHost() { auto* s = Session(); return s && s->role() == coop::net::Role::Host; }
bool IsClient() { auto* s = Session(); return s && s->role() == coop::net::Role::Client; }

uint64_t HashRows(const std::vector<SI::Row>& rows) {
    uint64_t h = 1469598103934665603ull;
    for (const SI::Row& r : rows) {
        const uint64_t n = r.bytes.size();
        h = coop::blob_chunks::Fnv64(reinterpret_cast<const uint8_t*>(&n), sizeof(n)) ^ (h * 1099511628211ull);
        for (uint8_t c : r.bytes) { h ^= c; h *= 1099511628211ull; }
    }
    return h;
}

// The host's watches: every change of a storage's rows ends in one of these two bodies.
void MarkComponent(void* comp) {
    if (!IsHost() || !SI::IsWorldStorage(comp)) return;
    W::Address a;
    if (AddressOf(comp, a)) g_dirty.insert(a);
}

void OnCalculatePost(const SG::Call& c) { MarkComponent(c.object); }

// A canvas unbinding its item widgets writes each widget's place into its row: the storage's rows may have moved.
void OnDespawnPost(const SG::Call& c) { MarkComponent(SI::CanvasStorage(c.object)); }

void OnTransferPost(const SG::Call& c) {
    MarkComponent(c.object);
    MarkComponent(SI::TransferCallTarget(c.locals, c.function));
}

// Read, page and send one storage to every world-ready peer (toSlot < 0) or to one slot. `force` sends an unchanged
// set (a joiner's seed). False when a send was refused: the caller keeps it marked.
bool Publish(const W::Address& a, int toSlot, bool force) {
    coop::net::Session* const s = Session();
    void* const comp = Resolve(a);
    if (!s || !comp) return true;   // gone: nothing to say
    std::vector<SI::Row> rows;
    if (!SI::ReadRows(comp, rows, W::kMaxRows)) {
        UE_LOGW("storage_contents[host]: %s does not read (plan %s, or over %zu rows) -- not published", Describe(a),
                IC::Ready() ? "ready" : "not measured", W::kMaxRows);
        return true;
    }
    Sent& sent = g_sent[a];
    const uint64_t h = HashRows(rows);
    bool changed = !sent.known || sent.hash != h;
    if (changed && toSlot >= 0) {
        // A change no drain has published yet goes to everyone first, so a joiner's seed never takes a revision the
        // others do not get.
        if (!Publish(a, -1, false)) return false;
        changed = !sent.known || sent.hash != h;
    }
    if (!changed && !force) return true;
    if (toSlot < 0 && !s->AnyWorldReadyPeer()) return true;   // nobody to tell; a joiner's seed reads it anew
    if (changed) {
        ++sent.revision;
        sent.hash = h;
        sent.known = true;
    }
    std::vector<std::vector<uint8_t>> encoded;
    encoded.reserve(rows.size());
    size_t bytes = 0;
    for (SI::Row& r : rows) { bytes += r.bytes.size(); encoded.push_back(std::move(r.bytes)); }
    std::vector<std::vector<uint8_t>> pages;
    if (!W::PackContents(a, sent.revision, IC::PlanSignature(), encoded, pages)) {
        UE_LOGW("storage_contents[host]: %s holds more than a set carries (%zu rows, %zu B) -- not published",
                Describe(a), encoded.size(), bytes);
        return true;
    }
    for (const auto& page : pages) {
        const bool ok = toSlot < 0
            ? coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::ContainerContents, g_nextSeq++, page)
            : coop::blob_chunks::SendBlobToSlot(s, toSlot, coop::net::ReliableKind::ContainerContents, g_nextSeq++,
                                                page);
        if (!ok) {
            sent.known = false;   // the retry sends the whole set again, under a fresh revision
            return false;
        }
    }
    if (toSlot < 0)
        UE_LOGI("storage_contents[host]: %s rev %u published (%zu rows, %zu B, %zu pages)", Describe(a),
                sent.revision, encoded.size(), bytes, pages.size());
    return true;
}

void Drain() {
    if (g_dirty.empty()) return;
    std::set<W::Address> refused;
    for (const W::Address& a : g_dirty)
        if (!Publish(a, -1, false)) refused.insert(a);
    g_dirty.swap(refused);
}

// Client: a whole set, applied when it is newer than the last, or held until the storage's actor is here.
enum class Applied : uint8_t { Done, Stale, NotHere, Failed };
Applied ApplySet(const W::Address& a, uint32_t revision, const std::vector<std::vector<uint8_t>>& rows) {
    const auto it = g_applied.find(a);
    if (it != g_applied.end() && revision <= it->second) return Applied::Stale;
    void* const comp = Resolve(a);
    if (!comp) return Applied::NotHere;
    const bool shown = SI::ShownStorage() == comp;
    g_inApply = true;
    const SI::ApplyResult r = SI::ApplyRows(comp, rows);
    g_inApply = false;
    if (r != SI::ApplyResult::Done) {
        UE_LOGW("storage_contents[client]: %s rev %u not applied (%s)", Describe(a), revision,
                r == SI::ApplyResult::Malformed ? "a row does not decode" : "the storage's verbs do not resolve");
        return Applied::Failed;
    }
    g_applied[a] = revision;
    UE_LOGI("storage_contents[client]: %s rev %u applied (%zu rows)%s", Describe(a), revision, rows.size(),
            shown ? ", the open screen redrawn" : "");
    return Applied::Done;
}

void OnPage(W::ContentsPage&& page) {
    if (page.plan != IC::PlanSignature()) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGE("storage_contents[client]: the host's row plan %08x is not this peer's %08x -- its storages are "
                    "not applied", page.plan, IC::PlanSignature());
        }
        return;
    }
    Assembly& as = g_assembling[page.address];
    if (page.page == 0) as = Assembly{page.revision, page.pages, 0, {}};
    if (as.revision != page.revision || as.pages != page.pages || as.next != page.page) {
        g_assembling.erase(page.address);   // a page out of its set: the next whole set replaces it
        return;
    }
    for (auto& r : page.rows) as.rows.push_back(std::move(r));
    if (as.rows.size() > W::kMaxRows) { g_assembling.erase(page.address); return; }
    if (++as.next < as.pages) return;
    Assembly done = std::move(as);
    g_assembling.erase(page.address);
    if (ApplySet(page.address, done.revision, done.rows) == Applied::NotHere) {
        Waiting& w = g_waiting[page.address];
        if (done.revision > w.revision) w = Waiting{done.revision, std::move(done.rows), Clock::now()};
    }
}

void TickWaiting() {
    const auto now = Clock::now();
    for (auto it = g_waiting.begin(); it != g_waiting.end();) {
        const Applied r = ApplySet(it->first, it->second.revision, it->second.rows);
        if (r == Applied::NotHere && now - it->second.since < kWaitBound) { ++it; continue; }
        if (r == Applied::NotHere)
            UE_LOGW("storage_contents[client]: %s rev %u dropped -- its storage never appeared here",
                    Describe(it->first), it->second.revision);
        it = g_waiting.erase(it);
    }
}

void RegisterWatches() {
    if (g_watchesRegistered) return;
    const bool a = SG::WatchName(kRowCalculateVolumes, kTagCalculate, nullptr, &OnCalculatePost);
    // The client's gate on the same body (storage_act) rides this row's watch.
    const bool b = SG::WatchName(kRowTransfer, kTagTransfer, &coop::props::storage_act::GateTransfer, &OnTransferPost);
    const bool d = SG::WatchName(kRowDespawn, kTagDespawn, nullptr, &OnDespawnPost);
    g_watchesRegistered = a && b && d;
    if (g_watchesRegistered) SG::ResolvePendingNames();
    else UE_LOGE("storage_contents: the storage watches were refused by the gate (calculateVolumes %d, "
                 "transferFromInvToInf %d, despawnCanvas %d) -- the host's storages are not published", a, b, d);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWatches();
}

void Tick() {
    const auto now = Clock::now();
    if (IsHost()) {
        if (now >= g_nextDronePoll) {
            g_nextDronePoll = now + kDronePollEvery;
            if (void* const drone = SI::DroneStorage(); drone && SI::IsWorldStorage(drone)) MarkComponent(drone);
        }
        if (now >= g_nextDrain) {
            g_nextDrain = now + kDrainEvery;
            Drain();
        }
    } else if (IsClient()) {
        if (!g_waiting.empty()) TickWaiting();
        if (!g_asm.Idle()) g_asm.Sweep(now, std::chrono::seconds(30));
    }
}

void OnDisconnect() {
    g_dirty.clear();
    g_sent.clear();
    g_applied.clear();
    g_assembling.clear();
    g_waiting.clear();
    g_asm.Clear();
    g_inApply = false;
}

void OnContentsChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!IsClient() || senderSlot != 0) return;   // only the host publishes a storage
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(p, senderSlot, blob)) return;
    W::ContentsPage page;
    if (!W::ParseContents(blob, page)) {
        UE_LOGW("storage_contents[client]: a malformed contents page (%zu B) dropped", blob.size());
        return;
    }
    OnPage(std::move(page));
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    if (!IsHost()) return;
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    size_t sent = 0, refused = 0;
    for (const auto& pr : pairs) {
        if (!pr.actor || !R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
        void* const comp = SI::StorageOf(pr.actor);
        if (!comp || !SI::IsWorldStorage(comp)) continue;
        const W::Address a{W::AddressKind::Prop, static_cast<uint32_t>(pr.id)};
        (Publish(a, peerSlot, true) ? sent : refused)++;
    }
    if (void* const drone = SI::DroneStorage(); drone && SI::IsWorldStorage(drone))
        (Publish(W::Address{W::AddressKind::Drone, 0}, peerSlot, true) ? sent : refused)++;
    UE_LOGI("storage_contents[host]: connect seed -> slot %d: %zu world storages sent, %zu refused", peerSlot, sent,
            refused);
}

void MarkDirty(void* component) { MarkComponent(component); }

void Resync(const W::Address& a) {
    if (!IsHost()) return;
    g_sent[a].known = false;   // the next publish reads it as changed: a new revision, to every world-ready peer
    g_dirty.insert(a);
}

bool AddressOf(void* component, W::Address& out) {
    if (!SI::IsWorldStorage(component)) return false;
    void* const owner = SI::OwnerOf(component);
    if (SI::IsDrone(owner)) {
        if (owner != ue_wrap::drone::Find()) return false;
        out = W::Address{W::AddressKind::Drone, 0};
        return true;
    }
    const auto eid = coop::element::Registry::Get().EidForActor(owner);
    if (eid == coop::element::kInvalidId || coop::element::LivePropActor(eid) != owner) return false;
    out = W::Address{W::AddressKind::Prop, static_cast<uint32_t>(eid)};
    return true;
}

void* Resolve(const W::Address& a) {
    void* comp = nullptr;
    if (a.kind == W::AddressKind::Drone) comp = SI::DroneStorage();
    else comp = SI::StorageOf(coop::element::LivePropActor(static_cast<coop::element::ElementId>(a.id)));
    return (comp && SI::IsWorldStorage(comp)) ? comp : nullptr;
}

uint32_t RevisionOf(const W::Address& a) {
    if (IsHost()) {
        const auto it = g_sent.find(a);
        return it == g_sent.end() ? 0 : it->second.revision;
    }
    const auto it = g_applied.find(a);
    return it == g_applied.end() ? 0 : it->second;
}

bool InApply() { return g_inApply; }

const char* Describe(const W::Address& a) {
    static char buf[32];
    if (a.kind == W::AddressKind::Drone) std::snprintf(buf, sizeof(buf), "drone");
    else std::snprintf(buf, sizeof(buf), "prop %u", a.id);
    return buf;
}

}  // namespace coop::props::storage_contents

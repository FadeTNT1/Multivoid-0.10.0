// coop/world/church_sync.cpp -- see coop/world/church_sync.h.
//
// Bytecode ground truth (outsideChurch_C, a0.10.0_0009):
//   - tryspawnherb (BeginPlay and every tick): while daynightCycle.rain >= 1 it spawns one of eight
//     growingplant_stolasherb_C at a random free slot, binds harvestedHerb to its `harvested` delegate and
//     keeps it in grownHerbs; when the rain falls to 0 it disintegrates every grownHerb and runs checkQuartz.
//     A herb fires `harvested` from dropFruits_grass, which its ReceiveDestroyed runs once grow >= 1, so a
//     wilting herb counts like a picked one.
//   - harvestedHerb: harvests++, updEyes; at >= 8: angry = true, a 2D scream, setAngry, setEvent(true).
//   - the lightning trigger's EndOverlap: OtherActor cast to mainPlayer_C, angry -> lightningStrike.
//   - lightningStrike: once (striked), spawns focusPlayer_C, after 0.15 s a lightningStrike_C at
//     strikeLocation, after 0.05 s more break(false), plays slomoTL, and setEvent(false) 10 s after the start.
//   - break(explode): once (broken): eight prop_C debris pieces, updBroken, audio and debris effects; with
//     explode an explosion_c. reachedByExplosion with the 'stolaslightning' tag calls break(true).
//   - checkQuartz can drop harvests and clear angry without a setEvent(false) (vanilla).
//   - truckLightning (a 20 s timer) -> fireLightningAtPlayer targets getPlayerPawn(0) when angry.
//   - getData saves mbool [broken, angry, striked] and mint [harvests]; loadData writes them back and runs
//     updBroken, updEyes.

#include "coop/world/church_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/world/spawn_authority.h"   // IsActiveClientSession

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <vector>

namespace coop::church_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace SG = ue_wrap::script_gate;
namespace E  = ue_wrap::engine;
namespace PN = ue_wrap::profile::name;

std::atomic<coop::net::Session*> g_session{nullptr};

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Host;
}

// ---- the watches ------------------------------------------------------------------------------
// The client refusals: every church verb that writes shared state or mints a shared object.
UE_FACT_FN(kRowHarvestedHerb, "church_sync", L"outsideChurch_C", L"harvestedHerb", Declared);
UE_FACT_FN(kRowLightningStrike, "church_sync", L"outsideChurch_C", L"lightningStrike", Declared);
UE_FACT_FN(kRowBreak, "church_sync", L"outsideChurch_C", L"break", Declared);
UE_FACT_FN(kRowTrySpawnHerb, "church_sync", L"outsideChurch_C", L"tryspawnherb", Declared);
UE_FACT_FN(kRowCheckQuartz, "church_sync", L"outsideChurch_C", L"checkQuartz", Declared);
UE_FACT_FN(kRowFireLightning, "church_sync", L"outsideChurch_C", L"fireLightningAtPlayer", Declared);
// The host's: the trigger stub (who struck), the strike's focus actor and the slow motion.
UE_FACT_FN(kRowTriggerEndOverlap, "church_sync", L"outsideChurch_C", L"BndEvt__outsideChurch_lightningTrigger_K2Node_ComponentBoundEvent_0_ComponentEndOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowSlomoUpdate, "church_sync", L"outsideChurch_C", L"slomoTL__UpdateFunc", Declared);
UE_FACT_FN(kRowFocusBeginPlay, "church_sync", L"focusPlayer_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowFocusTick, "church_sync", L"focusPlayer_C", L"ReceiveTick", Declared);

struct ClientRow { const ue_wrap::facts::Function* row; const char* tag; };
constexpr ClientRow kClientRows[] = {
    {&kRowHarvestedHerb,  "harvestedHerb"},
    {&kRowLightningStrike, "lightningStrike"},
    {&kRowBreak,          "break"},
    {&kRowTrySpawnHerb,   "tryspawnherb"},
    {&kRowCheckQuartz,    "checkQuartz"},
    {&kRowFireLightning,  "fireLightningAtPlayer"},
};
constexpr int kClientRowCount = static_cast<int>(std::size(kClientRows));
constexpr int kTagClientBase = 0x43480000;  // 'CH', then the row
constexpr int kTagEndOverlap = 0x43485430;  // 'CHT0'
constexpr int kTagSlomo      = 0x43485431;
constexpr int kTagFocusBegin = 0x43485432;
constexpr int kTagFocusTick  = 0x43485433;

uint64_t g_clientRefused[kClientRowCount] = {};

SG::Verdict OnClientRowPre(const SG::Call& call) {
    if (!coop::spawn_authority::IsActiveClientSession()) return SG::Verdict::Run;
    const int row = call.tag - kTagClientBase;
    if (row < 0 || row >= kClientRowCount) return SG::Verdict::Run;
    const uint64_t n = ++g_clientRefused[row];
    if ((n & (n - 1)) == 0)
        UE_LOGI("church_sync: client-refuse %s on %p (call #%llu) -- the host's church owns it", kClientRows[row].tag,
                call.object, static_cast<unsigned long long>(n));
    return SG::Verdict::Cancel;
}

// A puppet's strike on the host: set for the trigger stub's body, which spawns the focus actor synchronously.
bool g_puppetStrikeScope = false;
long long g_suppressSlomoUntilMs = 0;   // the slow motion of a strike a puppet made, refused until then
std::vector<ue_wrap::CachedObjRef> g_focusToRetire;
int32_t g_offOtherActor = -1;

SG::Verdict OnEndOverlapPre(const SG::Call& call) {
    if (!IsHost()) return SG::Verdict::Run;
    if (g_offOtherActor < 0 && call.function)
        g_offOtherActor = R::FindParamOffset(call.function, PN::OutsideChurch_EndOverlapOtherActorParam);
    if (g_offOtherActor < 0 || !call.locals) return SG::Verdict::Run;
    void* other = *reinterpret_cast<void* const*>(call.locals + g_offOtherActor);
    if (!other || !coop::players::Registry::Get().IsPuppetBody(other)) return SG::Verdict::Run;
    g_puppetStrikeScope = true;
    UE_LOGI("church_sync: peer %u's body left the church's lightning trigger on the host -- the native verb "
            "decides the strike; its camera focus and slow motion stay with that player",
            static_cast<unsigned>(coop::players::Registry::Get().PeerIdOfBody(other)));
    return SG::Verdict::Run;
}

void OnEndOverlapPost(const SG::Call&) {
    if (g_puppetStrikeScope) g_suppressSlomoUntilMs = NowMs() + 15000;  // the strike's 10 s plus the timeline
    g_puppetStrikeScope = false;
}

SG::Verdict OnSlomoPre(const SG::Call&) {
    return (IsHost() && NowMs() < g_suppressSlomoUntilMs) ? SG::Verdict::Cancel : SG::Verdict::Run;
}

bool IsRetiring(void* obj) {
    for (const auto& r : g_focusToRetire)
        if (r.Get() == obj) return true;
    return false;
}

SG::Verdict OnFocusBeginPre(const SG::Call& call) {
    if (!IsHost() || !g_puppetStrikeScope || !call.object) return SG::Verdict::Run;
    // The focus turns player 0, the host's own camera, toward a strike another player made: refuse its
    // input lock and look-at, stop its tick, and destroy it from the next Tick, outside its own spawn.
    E::SetActorTickEnabled(call.object, false);
    ue_wrap::CachedObjRef ref;
    ref.Set(call.object);
    g_focusToRetire.push_back(ref);
    UE_LOGI("church_sync: refused the strike's camera focus %p on the host (a puppet struck)", call.object);
    return SG::Verdict::Cancel;
}

SG::Verdict OnFocusTickPre(const SG::Call& call) {
    return (call.object && IsRetiring(call.object)) ? SG::Verdict::Cancel : SG::Verdict::Run;
}

bool g_watchesRegistered = false;
bool g_watchesSaid = false;

void RegisterWatches() {
    if (g_watchesRegistered) return;
    int ok = 0;
    for (int i = 0; i < kClientRowCount; ++i)
        ok += SG::WatchName(*kClientRows[i].row, kTagClientBase + i, &OnClientRowPre, nullptr) ? 1 : 0;
    ok += SG::WatchName(kRowTriggerEndOverlap, kTagEndOverlap, &OnEndOverlapPre, &OnEndOverlapPost) ? 1 : 0;
    ok += SG::WatchName(kRowSlomoUpdate, kTagSlomo, &OnSlomoPre, nullptr) ? 1 : 0;
    ok += SG::WatchName(kRowFocusBeginPlay, kTagFocusBegin, &OnFocusBeginPre, nullptr) ? 1 : 0;
    ok += SG::WatchName(kRowFocusTick, kTagFocusTick, &OnFocusTickPre, nullptr) ? 1 : 0;
    g_watchesRegistered = true;
    UE_LOGI("church_sync: %d/%d church watches registered by class and name (live once the names resolve)", ok,
            kClientRowCount + 4);
}

// ---- the church: layout and instance ---------------------------------------------------------
struct Layout {
    int32_t offBroken = -1, offAngry = -1, offStriked = -1, offHarvests = -1;
    uint8_t maskBroken = 0, maskAngry = 0, maskStriked = 0;
    void* updBroken = nullptr;
    void* updEyes = nullptr;
    bool complete() const {
        return offBroken >= 0 && offAngry >= 0 && offStriked >= 0 && offHarvests >= 0 && maskBroken && maskAngry &&
               maskStriked;
    }
};
Layout g_layout;
bool g_layoutFailed = false;
ue_wrap::CachedObjRef g_church;
long long g_nextSearchMs = 0;

void* Church() {
    if (void* c = g_church.Get()) return c;
    const long long now = NowMs();
    if (now < g_nextSearchMs || g_layoutFailed) return nullptr;
    g_nextSearchMs = now + 1000;
    void* cls = ue_wrap::object_index::ClassByName(PN::OutsideChurchClass);
    if (!cls) return nullptr;  // not loaded: the main menu, or a map without the church
    if (!g_layout.complete()) {
        R::FindBoolProperty(cls, PN::OutsideChurch_BrokenProp, g_layout.offBroken, g_layout.maskBroken);
        R::FindBoolProperty(cls, PN::OutsideChurch_AngryProp, g_layout.offAngry, g_layout.maskAngry);
        R::FindBoolProperty(cls, PN::OutsideChurch_StrikedProp, g_layout.offStriked, g_layout.maskStriked);
        g_layout.offHarvests = R::FindPropertyOffset(cls, PN::OutsideChurch_HarvestsProp);
        g_layout.updBroken = R::FindFunction(cls, PN::OutsideChurch_UpdBrokenFn);
        g_layout.updEyes = R::FindFunction(cls, PN::OutsideChurch_UpdEyesFn);
        if (!g_layout.complete()) {
            g_layoutFailed = true;
            UE_LOGW("church_sync: outsideChurch_C's fields did not resolve (broken=%d angry=%d striked=%d "
                    "harvests=%d) -- the church is not synced this process",
                    g_layout.offBroken, g_layout.offAngry, g_layout.offStriked, g_layout.offHarvests);
            return nullptr;
        }
    }
    void* found = nullptr;
    ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t index) {
        auto& out = *static_cast<void**>(ctx);
        if (out || !obj) return;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        if (!R::IsLive(obj)) return;
        if (R::SlotFlags(index) & R::slot_flags::NotYetReadable) return;
        out = obj;
    }, &found);
    if (found) {
        g_church.Set(found);
        UE_LOGI("church_sync: bound outsideChurch_C %p", found);
    }
    return found;
}

bool ReadBit(void* obj, int32_t off, uint8_t mask) {
    return (*(reinterpret_cast<const uint8_t*>(obj) + off) & mask) != 0;
}

void WriteBit(void* obj, int32_t off, uint8_t mask, bool v) {
    uint8_t* b = reinterpret_cast<uint8_t*>(obj) + off;
    *b = v ? static_cast<uint8_t>(*b | mask) : static_cast<uint8_t>(*b & ~mask);
}

coop::net::ChurchStatePayload Read(void* church) {
    coop::net::ChurchStatePayload p{};
    p.flags = static_cast<uint8_t>((ReadBit(church, g_layout.offBroken, g_layout.maskBroken) ? 1 : 0) |
                                   (ReadBit(church, g_layout.offAngry, g_layout.maskAngry) ? 2 : 0) |
                                   (ReadBit(church, g_layout.offStriked, g_layout.maskStriked) ? 4 : 0));
    const int32_t h = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(church) + g_layout.offHarvests);
    p.harvests = static_cast<uint8_t>(h < 0 ? 0 : (h > 255 ? 255 : h));
    return p;
}

void CallNoParams(void* obj, void* fn) {
    if (!fn) return;
    ue_wrap::ParamFrame f(fn);
    if (f.valid()) ue_wrap::Call(obj, f);
}

// ---- host poll, client apply ------------------------------------------------------------------
long long g_lastPollMs = 0;
int g_lastSent = -1;          // flags | harvests << 8; -1 = prime (a prime never sends)
void* g_polled = nullptr;     // the church the baseline belongs to
int g_pending = -1;           // a client's state that arrived before its church was bound

int Pack(const coop::net::ChurchStatePayload& p) { return p.flags | (p.harvests << 8); }

void Apply(int packed) {
    void* church = Church();
    if (!church) {
        g_pending = packed;
        return;
    }
    g_pending = -1;
    const coop::net::ChurchStatePayload before = Read(church);
    const bool broken = (packed & 1) != 0;
    WriteBit(church, g_layout.offBroken, g_layout.maskBroken, broken);
    WriteBit(church, g_layout.offAngry, g_layout.maskAngry, (packed & 2) != 0);
    WriteBit(church, g_layout.offStriked, g_layout.maskStriked, (packed & 4) != 0);
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(church) + g_layout.offHarvests) = (packed >> 8) & 0xFF;
    // The pair loadData runs after writing these fields: the gibs and the eyes follow the state.
    if (((before.flags & 1) != 0) != broken) CallNoParams(church, g_layout.updBroken);
    CallNoParams(church, g_layout.updEyes);
    UE_LOGI("church_sync: applied the host's church broken=%d angry=%d striked=%d harvests=%d", broken ? 1 : 0,
            (packed >> 1) & 1, (packed >> 2) & 1, (packed >> 8) & 0xFF);
}

void HostPoll(coop::net::Session& s) {
    void* church = Church();
    if (!church) return;
    const coop::net::ChurchStatePayload p = Read(church);
    const int cur = Pack(p);
    if (church != g_polled || g_lastSent < 0) {
        g_polled = church;
        g_lastSent = cur;
        return;
    }
    if (cur == g_lastSent) return;
    g_lastSent = cur;
    if (s.SendReliable(coop::net::ReliableKind::ChurchState, &p, sizeof(p)))
        UE_LOGI("church_sync: host broadcast broken=%d angry=%d striked=%d harvests=%u", p.flags & 1,
                (p.flags >> 1) & 1, (p.flags >> 2) & 1, static_cast<unsigned>(p.harvests));
    else
        UE_LOGW("church_sync: host broadcast send FAILED");
}

void RetireFocus() {
    for (auto& r : g_focusToRetire)
        if (void* a = r.Get()) E::DestroyActor(a);
    g_focusToRetire.clear();
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWatches();
}

void Tick() {
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (!g_watchesSaid && g_watchesRegistered) {
        SG::ResolvePendingNames();
        g_watchesSaid = true;
    }
    if (!g_focusToRetire.empty() && !g_puppetStrikeScope) RetireFocus();
    const long long now = NowMs();
    if (now - g_lastPollMs < 1000) return;
    g_lastPollMs = now;
    if (s->role() == coop::net::Role::Host) {
        HostPoll(*s);
    } else if (g_pending >= 0) {
        Apply(g_pending);
    }
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    g_nextSearchMs = 0;  // one bound now, not after a miss latched a moment ago
    void* church = Church();
    if (!church) {
        UE_LOGI("church_sync: connect-snapshot slot=%d -- no church in this world", slot);
        return;
    }
    const coop::net::ChurchStatePayload p = Read(church);
    if (s->SendReliableToSlot(slot, coop::net::ReliableKind::ChurchState, &p, sizeof(p)))
        UE_LOGI("church_sync: connect-snapshot -- sent flags=0x%02X harvests=%u to slot %d",
                static_cast<unsigned>(p.flags), static_cast<unsigned>(p.harvests), slot);
    else
        UE_LOGW("church_sync: connect-snapshot to slot %d send FAILED", slot);
}

void OnReliable(const coop::net::ChurchStatePayload& payload) {
    if (!GT::IsGameThread()) {
        UE_LOGW("church_sync: OnReliable off-game-thread -- dropping");
        return;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client) return;
    Apply(Pack(payload));
}

void OnDisconnect() {
    g_church.Reset();
    g_nextSearchMs = 0;
    g_lastPollMs = 0;
    g_lastSent = -1;
    g_polled = nullptr;
    g_pending = -1;
    g_puppetStrikeScope = false;
    g_suppressSlomoUntilMs = 0;
    g_focusToRetire.clear();
    g_watchesSaid = false;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::church_sync

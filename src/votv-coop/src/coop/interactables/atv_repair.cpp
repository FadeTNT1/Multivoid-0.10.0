// coop/interactables/atv_repair.cpp -- see coop/interactables/atv_repair.h.

#include "coop/interactables/atv_repair.h"

#include "coop/element/intent_authority.h"
#include "coop/interactables/atv_sync.h"
#include "coop/interactables/atv_sync_internal.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/devices/atv_condition.h"
#include "ue_wrap/devices/atv_repair.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::atv_repair {
namespace {

namespace A   = ue_wrap::atv;
namespace AC  = ue_wrap::atv_condition;
namespace AR  = ue_wrap::atv_repair;
namespace EL  = coop::element;
namespace SG  = ue_wrap::script_gate;
namespace R   = ue_wrap::reflection;
using coop::net::AtvRepairOp;

// ATV_C's own bodies; car1_witch_C inherits them, so a Declared row matches it too.
UE_FACT_FN(kRowUsedOn, "atv_repair", L"ATV_C", L"playerUsedOn", Declared);
UE_FACT_FN(kRowPutTire, "atv_repair", L"ATV_C", L"putTire", Declared);
UE_FACT_FN(kRowUnscrew, "atv_repair", L"ATV_C", L"unscrewPanel", Declared);
UE_FACT_FN(kRowFuelUp, "atv_repair", L"ATV_C", L"fuelUp", Declared);
UE_FACT_FN(kRowFix, "atv_repair", L"ATV_C", L"toolboxFix", Declared);

constexpr int kTagUsedOn  = 0x41545230;  // 'ATR0'
constexpr int kTagPutTire = 0x41545231;
constexpr int kTagUnscrew = 0x41545232;
constexpr int kTagFuelUp  = 0x41545233;
constexpr int kTagFix     = 0x41545234;

// The seat's reach (atv_authority.cpp): Authorize adds the ATV's bounds and the pose-staleness pad.
constexpr float kReachUU = 400.f;
constexpr coop::net::IntentBudget kBudget{6.0f, 3.0f};
// An unscrew the client cancelled keeps its progress until the deferred resetUnscrew lands, and the game's tick asks
// again each frame meanwhile; the same unscrew is sent once in this window.
constexpr uint64_t kUnscrewPaceMs = 1000;

std::atomic<coop::net::Session*> g_session{nullptr};

enum Row { kUsedOn, kPutTire, kUnscrew, kFuelUp, kFix, kRows };

SG::Verdict OnUsedOnPre(const SG::Call& c);
void OnUsedOnPost(const SG::Call& c);
SG::Verdict OnPutTirePre(const SG::Call& c);
void OnPutTirePost(const SG::Call& c);
SG::Verdict OnUnscrewPre(const SG::Call& c);
void OnUnscrewPost(const SG::Call& c);
SG::Verdict OnFuelUpPre(const SG::Call& c);
void OnFuelUpPost(const SG::Call& c);
SG::Verdict OnFixPre(const SG::Call& c);
void OnFixPost(const SG::Call& c);

struct Hook {
    const ue_wrap::facts::Function* row;
    int                             tag;
    SG::PreFn                       pre;
    SG::PostFn                      post;
    const wchar_t*                  name;
};
const Hook kTable[kRows] = {
    {&kRowUsedOn, kTagUsedOn, &OnUsedOnPre, &OnUsedOnPost, L"playerUsedOn"},
    {&kRowPutTire, kTagPutTire, &OnPutTirePre, &OnPutTirePost, L"putTire"},
    {&kRowUnscrew, kTagUnscrew, &OnUnscrewPre, &OnUnscrewPost, L"unscrewPanel"},
    {&kRowFuelUp, kTagFuelUp, &OnFuelUpPre, &OnFuelUpPost, L"fuelUp"},
    {&kRowFix, kTagFix, &OnFixPre, &OnFixPost, L"toolboxFix"},
};

constexpr auto kRetry = std::chrono::seconds(1);
constexpr int  kMaxTries = 30;
using Clock = std::chrono::steady_clock;
bool              g_done[kRows] = {};
bool              g_dead[kRows] = {};
int               g_tries[kRows] = {};
Clock::time_point g_nextTry{};
bool              g_saidLive = false;

// CLIENT: each row's PRE snapshot of the copy it ran on, read back by its POST.
struct Pre {
    void*        atv = nullptr;
    AC::Snapshot snap{};
    bool         ok = false;
};
Pre g_pre[kRows];

R::ParamOffset g_usedOnPlayer{L"player"};

// CLIENT: the last unscrew sent, for its pace.
struct LastUnscrew { void* atv = nullptr; int8_t index = -2; uint64_t ms = 0; };
LastUnscrew g_lastUnscrew;

// HOST: the copies a repair changed since atv_sync's Tick last asked.
std::vector<void*> g_due;
coop::net::IntentBucket g_buckets[coop::net::kMaxPeers];

uint64_t g_sent[8] = {}, g_applied[8] = {}, g_refused[8] = {}, g_hostNative = 0;

const char* OpName(uint8_t op) {
    switch (static_cast<AtvRepairOp>(op)) {
    case AtvRepairOp::BatteryIn: return "battery-in";
    case AtvRepairOp::SpareIn:   return "spare-in";
    case AtvRepairOp::PutTire:   return "put-wheel";
    case AtvRepairOp::Unscrew:   return "unscrew";
    case AtvRepairOp::Fuel:      return "fuel";
    case AtvRepairOp::Fix:       return "toolbox-fix";
    }
    return "unknown";
}

uint8_t OpIndex(uint8_t op) { return op < 8 ? op : 0; }

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void MarkDue(void* atv) {
    if (atv && std::find(g_due.begin(), g_due.end(), atv) == g_due.end()) g_due.push_back(atv);
}

coop::net::Session* Sess() { return g_session.load(std::memory_order_acquire); }

bool IsHost(coop::net::Session* s) {
    return s && s->running() && s->connected() && s->role() == coop::net::Role::Host;
}

// A client whose world is ready, on an ATV the lane holds: the only copies whose repair is sent.
bool ClientCopy(coop::net::Session* s, void* atv) {
    return s && s->running() && s->connected() && s->role() == coop::net::Role::Client && atv &&
           coop::net_pump::HasAnnouncedWorldReady() && coop::atv_sync::IsIndexed(atv);
}

void Send(coop::net::Session& s, void* atv, AtvRepairOp op, int8_t index, float value, float dirt, int8_t fixes) {
    const std::wstring key = A::GetKeyString(atv);
    if (key.empty() || key == L"None") return;
    coop::net::AtvRepairIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.op = static_cast<uint8_t>(op);
    p.index = index;
    p.fixes = fixes;
    p.value = value;
    p.dirt = dirt;
    if (!s.SendReliable(coop::net::ReliableKind::AtvRepairIntent, &p, sizeof(p))) {
        UE_LOGW("[ATV-REPAIR] client: %s on '%ls' was not sent -- it stays on this copy until the host's next block",
                OpName(p.op), key.c_str());
        return;
    }
    ++g_sent[OpIndex(p.op)];
    UE_LOGI("[ATV-REPAIR] client: sent %s on '%ls' (index %d, value %.1f, dirt %.2f, fixes %d)", OpName(p.op),
            key.c_str(), static_cast<int>(index), value, dirt, static_cast<int>(fixes));
}

void TakePre(Row row, void* atv) {
    Pre& pre = g_pre[row];
    pre.atv = atv;
    pre.ok = AC::Read(atv, pre.snap);
}

// The PRE's snapshot for this POST, consumed; false when there is none for `atv`.
bool UsePre(Row row, void* atv, AC::Snapshot& before) {
    Pre& pre = g_pre[row];
    const bool ok = pre.ok && pre.atv == atv;
    if (ok) before = pre.snap;
    pre = Pre{};
    return ok;
}

// ---- the watches ----------------------------------------------------------------------------------------------

// playerUsedOn: a battery or a spare fitted from the hand (the item is the client's own; its body runs here).
SG::Verdict OnUsedOnPre(const SG::Call& c) {
    if (!ClientCopy(Sess(), c.object)) return SG::Verdict::Run;
    const int32_t off = g_usedOnPlayer.Of(c.function);
    void* player = (off >= 0 && c.locals) ? *reinterpret_cast<void* const*>(c.locals + off) : nullptr;
    if (player && !coop::players::Registry::Get().IsLocal(player)) return SG::Verdict::Run;
    TakePre(kUsedOn, c.object);
    return SG::Verdict::Run;
}

void OnUsedOnPost(const SG::Call& c) {
    coop::net::Session* s = Sess();
    if (IsHost(s)) { if (c.object) { MarkDue(c.object); ++g_hostNative; } return; }
    if (!ClientCopy(s, c.object)) return;
    AC::Snapshot before, after;
    if (!UsePre(kUsedOn, c.object, before) || !AC::Read(c.object, after)) return;
    if (before.battery < 0.f && after.battery >= 0.f)
        Send(*s, c.object, AtvRepairOp::BatteryIn, -1, after.battery, 0.f, 0);
    if (!before.hasSpare && after.hasSpare)
        Send(*s, c.object, AtvRepairOp::SpareIn, -1, after.spareDur, after.spareDirt, after.spareFixes);
}

// putTire: a wheel from the hand into an empty socket, from playerUsedOn or from an unscrew of a socket.
SG::Verdict OnPutTirePre(const SG::Call& c) {
    if (ClientCopy(Sess(), c.object)) TakePre(kPutTire, c.object);
    return SG::Verdict::Run;
}

void OnPutTirePost(const SG::Call& c) {
    coop::net::Session* s = Sess();
    if (IsHost(s)) { if (c.object) { MarkDue(c.object); ++g_hostNative; } return; }
    if (!ClientCopy(s, c.object)) return;
    AC::Snapshot before, after;
    if (!UsePre(kPutTire, c.object, before) || !AC::Read(c.object, after)) return;
    for (int i = 0; i < 4; ++i) {
        const uint8_t bit = static_cast<uint8_t>(1u << i);
        if ((after.mask & bit) && !(before.mask & bit))
            Send(*s, c.object, AtvRepairOp::PutTire, static_cast<int8_t>(i), after.dur[i], after.dirt[i],
                 after.fixes[i]);
    }
}

// unscrewPanel: a wheel or the battery comes off as a prop, which only the host may birth. A client's is cancelled
// and asked of the host; an unscrew into an empty socket (putTire with the wheel in the hand) runs here.
SG::Verdict OnUnscrewPre(const SG::Call& c) {
    coop::net::Session* s = Sess();
    if (!ClientCopy(s, c.object)) return SG::Verdict::Run;
    AR::Look look;
    if (!AR::ReadLook(c.object, look)) return SG::Verdict::Run;
    int8_t index = -2;
    if (look.tire >= 0 && look.tire <= 3) {
        index = static_cast<int8_t>(look.tire);
    } else if (look.socket < 0) {
        float battery = -1.f;
        if (AC::ReadBattery(c.object, battery) && battery >= 0.f) index = -1;
    }
    if (index == -2) return SG::Verdict::Run;
    const uint64_t now = coop::net::NowMs();
    if (!(g_lastUnscrew.atv == c.object && g_lastUnscrew.index == index && now - g_lastUnscrew.ms < kUnscrewPaceMs)) {
        g_lastUnscrew = LastUnscrew{c.object, index, now};
        Send(*s, c.object, AtvRepairOp::Unscrew, index, 0.f, 0.f, 0);
    }
    // The game's own unscrewPanel would have ended the unscrew; a frame later, outside this body, the client's ends it.
    ue_wrap::CachedObjRef ref;
    ref.Set(c.object);
    ue_wrap::game_thread::Post([ref]() {
        if (void* atv = ref.Get()) AR::ResetUnscrew(atv);
    });
    return SG::Verdict::Cancel;
}

void OnUnscrewPost(const SG::Call& c) {
    if (IsHost(Sess()) && c.object) { MarkDue(c.object); ++g_hostNative; }
}

// fuelUp: a gas can's refuel (the can is the client's own).
SG::Verdict OnFuelUpPre(const SG::Call& c) {
    if (ClientCopy(Sess(), c.object)) TakePre(kFuelUp, c.object);
    return SG::Verdict::Run;
}

void OnFuelUpPost(const SG::Call& c) {
    coop::net::Session* s = Sess();
    if (IsHost(s)) { if (c.object) { MarkDue(c.object); ++g_hostNative; } return; }
    if (!ClientCopy(s, c.object)) return;
    AC::Snapshot before, after;
    if (!UsePre(kFuelUp, c.object, before) || !AC::Read(c.object, after)) return;
    if (after.fuel > before.fuel + 0.01f) Send(*s, c.object, AtvRepairOp::Fuel, -1, after.fuel, 0.f, 0);
}

// toolboxFix: health to full (the toolbox is the client's own).
SG::Verdict OnFixPre(const SG::Call& c) {
    if (ClientCopy(Sess(), c.object)) TakePre(kFix, c.object);
    return SG::Verdict::Run;
}

void OnFixPost(const SG::Call& c) {
    coop::net::Session* s = Sess();
    if (IsHost(s)) { if (c.object) { MarkDue(c.object); ++g_hostNative; } return; }
    if (!ClientCopy(s, c.object)) return;
    AC::Snapshot before, after;
    if (!UsePre(kFix, c.object, before) || !AC::Read(c.object, after)) return;
    if (after.health > before.health + 0.01f) Send(*s, c.object, AtvRepairOp::Fix, -1, after.health, 0.f, 0);
}

// ---- the host's judgement ---------------------------------------------------------------------------------------

// The reason the request does not fit the host's copy, or null when it was written or run.
const char* Apply(void* atv, const coop::net::AtvRepairIntentPayload& p, std::string& said) {
    AC::Snapshot snap;
    if (!AC::Read(atv, snap)) return "the host's copy did not read";
    if (!std::isfinite(p.value) || !std::isfinite(p.dirt)) return "a non-finite value";
    char buf[160];
    switch (static_cast<AtvRepairOp>(p.op)) {
    case AtvRepairOp::BatteryIn: {
        if (snap.battery >= 0.f) return "a battery is in already";
        const float v = Clamp(p.value, 0.f, 100.f);
        if (!AC::WriteBattery(atv, v)) return "the battery did not write";
        AC::CallVerb(atv, AC::Verb::UpdBattery);
        std::snprintf(buf, sizeof(buf), "battery %.1f -> %.1f", snap.battery, v);
        break;
    }
    case AtvRepairOp::SpareIn: {
        if (snap.hasSpare) return "a spare is in already";
        const float was = snap.spareDur;
        snap.spareDur = Clamp(p.value, 0.f, 100.f);
        snap.spareDirt = Clamp(p.dirt, 0.f, 1.f);
        snap.spareFixes = p.fixes;
        AC::WriteAccumulators(atv, snap);
        AC::WritePresence(atv, snap.mask, true);
        AC::CallVerb(atv, AC::Verb::UpdSpareTire);
        std::snprintf(buf, sizeof(buf), "spare in (durability %.1f, was %.1f)", snap.spareDur, was);
        break;
    }
    case AtvRepairOp::PutTire: {
        if (p.index < 0 || p.index > 3) return "no such socket";
        const uint8_t bit = static_cast<uint8_t>(1u << p.index);
        if (snap.mask & bit) return "the socket holds a wheel already";
        if (!(p.value > 0.f)) return "a broken wheel does not fit";
        snap.dur[p.index] = Clamp(p.value, 0.f, 100.f);
        snap.dirt[p.index] = Clamp(p.dirt, 0.f, 1.f);
        snap.fixes[p.index] = p.fixes;
        AC::WriteAccumulators(atv, snap);
        AC::WritePresence(atv, static_cast<uint8_t>(snap.mask | bit), snap.hasSpare);
        AC::CallVerb(atv, AC::Verb::UpdTires);
        std::snprintf(buf, sizeof(buf), "wheel %d in (durability %.1f), mask %02X -> %02X", p.index,
                      snap.dur[p.index], snap.mask, static_cast<unsigned>(snap.mask | bit));
        break;
    }
    case AtvRepairOp::Unscrew: {
        if (p.index < -1 || p.index > 3) return "no such wheel";
        if (p.index >= 0 && !(snap.mask & (1u << p.index))) return "that wheel is off already";
        if (p.index < 0 && snap.battery < 0.f) return "no battery is in";
        if (!AR::ReplayUnscrew(atv, p.index)) return "the game's unscrew did not run on the host's copy";
        if (p.index >= 0) std::snprintf(buf, sizeof(buf), "wheel %d off", p.index);
        else std::snprintf(buf, sizeof(buf), "battery out (charge %.1f)", snap.battery);
        break;
    }
    case AtvRepairOp::Fuel: {
        const float v = Clamp(p.value, 0.f, 100.f);
        if (v <= snap.fuel + 0.01f) return "the host's copy holds as much fuel";
        const float was = snap.fuel;
        snap.fuel = v;
        AC::WriteAccumulators(atv, snap);
        A::WriteEmptyLatch(atv, false);
        std::snprintf(buf, sizeof(buf), "fuel %.1f -> %.1f", was, v);
        break;
    }
    case AtvRepairOp::Fix: {
        if (snap.health >= 100.f) return "the host's copy is at full health";
        const float was = snap.health;
        snap.health = 100.f;
        AC::WriteAccumulators(atv, snap);
        A::WriteBrokenLatch(atv, false);
        AC::CallVerb(atv, AC::Verb::UpdHealth);
        std::snprintf(buf, sizeof(buf), "health %.1f -> 100", was);
        break;
    }
    default:
        return "an unknown op";
    }
    said = buf;
    return nullptr;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_saidLive || Clock::now() < g_nextTry) return;
    g_nextTry = Clock::now() + kRetry;
    bool pending = false;
    for (int i = 0; i < kRows; ++i) {
        if (g_done[i] || g_dead[i]) continue;
        if (SG::WatchName(*kTable[i].row, kTable[i].tag, kTable[i].pre, kTable[i].post)) {
            g_done[i] = true;
        } else if (++g_tries[i] >= kMaxTries) {
            g_dead[i] = true;
            UE_LOGE("[ATV-REPAIR] the ATV_C::%ls watch is dead -- that repair stays on the copy it was made on",
                    kTable[i].name);
        } else {
            pending = true;
        }
    }
    if (pending) return;
    SG::ResolvePendingNames();
    for (int i = 0; i < kRows; ++i)
        if (g_done[i] && !SG::NameWatchLive(*kTable[i].row, kTable[i].tag)) return;   // asked again next second
    g_saidLive = true;
    UE_LOGI("[ATV-REPAIR] the ATV_C playerUsedOn, putTire, unscrewPanel, fuelUp and toolboxFix watches are live");
}

void OnIntent(coop::net::Session& session, const coop::net::AtvRepairIntentPayload& p, uint8_t slot) {
    if (session.role() != coop::net::Role::Host || slot < 1 || slot >= coop::net::kMaxPeers) return;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    const uint8_t oi = OpIndex(p.op);
    auto refuse = [&](const char* why, void* target) {
        ++g_refused[oi];
        UE_LOGI("[ATV-REPAIR] host: REFUSED slot %u's %s on '%ls' (index %d) -- %s; its copy is set back by the "
                "host's block",
                static_cast<unsigned>(slot), OpName(p.op), key.c_str(), static_cast<int>(p.index), why);
        MarkDue(target);
    };
    if (!g_buckets[slot].Take(kBudget, coop::net::NowMs())) { refuse("paced", nullptr); return; }
    const coop::atv_sync::AtvEntry* e = coop::atv_sync::FindEntry(key);
    if (!e || !R::IsLiveByIndex(e->actor, e->idx)) { refuse("no ATV holds the key", nullptr); return; }
    void* atv = e->actor;
    const EL::IntentSubject subject = EL::IntentTarget::ForClientIntent(session, slot, kReachUU).Authorize(atv);
    if (!subject) { refuse(EL::OutcomeName(subject.outcome), atv); return; }
    std::string said;
    if (const char* why = Apply(atv, p, said)) { refuse(why, atv); return; }
    ++g_applied[oi];
    MarkDue(atv);
    UE_LOGI("[ATV-REPAIR] host: applied slot %u's %s on '%ls' -- %s", static_cast<unsigned>(slot), OpName(p.op),
            key.c_str(), said.c_str());
}

bool TakeDue(void* actor) {
    auto it = std::find(g_due.begin(), g_due.end(), actor);
    if (it == g_due.end()) return false;
    g_due.erase(it);
    return true;
}

void Reset() {
    uint64_t sent = 0, applied = 0, refused = 0;
    for (int i = 0; i < 8; ++i) { sent += g_sent[i]; applied += g_applied[i]; refused += g_refused[i]; }
    if (sent || applied || refused || g_hostNative)
        UE_LOGI("[ATV-REPAIR] session -- client sent %llu; host applied %llu, refused %llu, own repairs %llu",
                static_cast<unsigned long long>(sent), static_cast<unsigned long long>(applied),
                static_cast<unsigned long long>(refused), static_cast<unsigned long long>(g_hostNative));
    for (int i = 0; i < 8; ++i) g_sent[i] = g_applied[i] = g_refused[i] = 0;
    g_hostNative = 0;
    g_due.clear();
    for (auto& b : g_buckets) b.Reset();
    for (auto& pre : g_pre) pre = Pre{};
    g_lastUnscrew = LastUnscrew{};
}

}  // namespace coop::atv_repair

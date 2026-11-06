// coop/interactables/microwave_sync.cpp -- see coop/interactables/microwave_sync.h.

#include "coop/interactables/microwave_sync.h"

#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_key_index.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/devices/appliance.h"
#include "ue_wrap/devices/microwave.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coop::microwave_sync {
namespace {

namespace EL = coop::element;
namespace MW = ue_wrap::microwave;
namespace MS = coop::net::microwave_state_flags;
namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowAction, "microwave_sync", L"prop_microwave_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowWave, "microwave_sync", L"prop_microwave_C", L"wave", Declared);
UE_FACT_FN(kRowTick, "microwave_sync", L"prop_microwave_C", L"ReceiveTick", Declared);
// What the loop calls on each of the objects inside, on every class that implements it.
UE_FACT_FN(kRowHeat, "microwave_sync", nullptr, L"microwave", AnyClass);
UE_FACT_FN(kRowElec, "microwave_sync", nullptr, L"microwaveElec", AnyClass);
constexpr int kTagAction = 0x4D575031;   // 'MWP1'
constexpr int kTagWave = 0x4D575032;
constexpr int kTagTick = 0x4D575033;
constexpr int kTagHeat = 0x4D575034;
constexpr int kTagElec = 0x4D575035;

struct Watch {
    const ue_wrap::facts::Function* row;
    int                             tag;
    const char*                     what;
};
const Watch kWatches[] = {
    {&kRowAction, kTagAction, "actionOptionIndex"}, {&kRowWave, kTagWave, "wave"},
    {&kRowTick, kTagTick, "ReceiveTick"},           {&kRowHeat, kTagHeat, "microwave"},
    {&kRowElec, kTagElec, "microwaveElec"},
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;

constexpr float    kReachUU = 450.0f;
constexpr coop::net::IntentBudget kBudget{4.0f, 4.0f};
constexpr size_t   kMaxPending = 8;
constexpr uint64_t kRunPeriodMs = 1000;
constexpr uint64_t kSayMs = 10000;
constexpr size_t   kMaxMicrowaves = 64;

std::atomic<coop::net::Session*> g_session{nullptr};

// The client's own press the lane let run (its send failed): the bit its PRE put on the press's scope, so the wave
// it calls runs too.
uintptr_t g_localBit = 0;
void*     g_actionFn = nullptr;

R::ParamOffset g_actionParam{L"action"};
R::ParamOffset g_playerParam{L"player"};

uint64_t g_sent = 0, g_ran = 0, g_denied = 0, g_dropped = 0, g_published = 0, g_applied = 0, g_cancelledWave = 0,
         g_cancelledEffects = 0, g_unknown = 0;
bool g_waveSaid = false, g_effectSaid = false, g_publishSaid = false, g_fullSaid = false;

uint64_t NowMs() { return coop::net::NowMs(); }

bool IsClient(coop::net::Session* s) {
    return s && s->running() && s->connected() && s->role() == coop::net::Role::Client;
}
bool IsHost(coop::net::Session* s) { return s && s->running() && s->role() == coop::net::Role::Host; }

// ---- host: the noted microwaves -----------------------------------------------------------------------------------
struct HostRec {
    ue_wrap::CachedObjRef actor;
    MW::State             last;
    bool                  sent = false;
    bool                  dirty = false;
    uint64_t              sentMs = 0;
};
std::vector<HostRec> g_host;

HostRec* Note(void* mw) {
    for (HostRec& r : g_host)
        if (r.actor.Is(mw)) return &r;
    for (HostRec& r : g_host)
        if (!r.actor.Alive()) {
            r = HostRec{};
            r.actor.Set(mw);
            return &r;
        }
    if (g_host.size() >= kMaxMicrowaves) {
        if (!g_fullSaid) {
            g_fullSaid = true;
            UE_LOGW("[MICROWAVE] host: %zu microwaves noted already -- a further one is not published (said once)",
                    kMaxMicrowaves);
        }
        return nullptr;
    }
    HostRec r;
    r.actor.Set(mw);
    g_host.push_back(std::move(r));
    return &g_host.back();
}

void MarkDirty(void* mw) {
    if (HostRec* r = Note(mw)) r->dirty = true;
}

bool Fill(void* mw, const MW::State& st, bool ding, coop::net::MicrowaveStatePayload& p) {
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(mw);
    if (key.empty() || key == L"None" || key.size() > sizeof(p.key.data)) return false;
    p = coop::net::MicrowaveStatePayload{};
    coop::net::WireKeyFromString(key, p.key);
    p.timer = st.timer;
    p.remaining = st.time < 0.f ? 0.f : st.time;
    p.timeMode = st.timeMode;
    p.flags = static_cast<uint8_t>((st.active ? MS::kActive : 0) | (ding ? MS::kDing : 0));
    return true;
}

void PumpHost(coop::net::Session& s) {
    if (g_host.empty() || !s.connected() || !s.AnyWorldReadyPeer()) return;
    const uint64_t now = NowMs();
    for (HostRec& r : g_host) {
        void* mw = r.actor.Get();
        MW::State st;
        if (!mw || !MW::ReadState(mw, st)) continue;
        const bool changed = !r.sent || r.dirty || st.active != r.last.active || st.timeMode != r.last.timeMode ||
                             st.timer != r.last.timer;
        if (!changed && !(st.active && now - r.sentMs >= kRunPeriodMs)) continue;
        const bool ding = r.sent && r.last.active && !st.active;
        coop::net::MicrowaveStatePayload p{};
        if (!Fill(mw, st, ding, p)) {
            r.sent = true;
            r.dirty = false;
            r.last = st;
            r.sentMs = now;
            continue;
        }
        if (!s.SendReliable(coop::net::ReliableKind::MicrowaveState, &p, sizeof(p))) continue;
        ++g_published;
        if (changed && (!g_publishSaid || st.active != r.last.active)) {
            g_publishSaid = true;
            UE_LOGI("[MICROWAVE] host: key='%ls' %s mode=%d timer=%.0f left=%.1f%s",
                    coop::net::StringFromWireKey(p.key).c_str(), st.active ? "RUNNING" : "idle", st.timeMode,
                    static_cast<double>(st.timer), static_cast<double>(p.remaining), ding ? " (ding)" : "");
        }
        r.sent = true;
        r.dirty = false;
        r.last = st;
        r.sentMs = now;
    }
}

// ---- host: the presses --------------------------------------------------------------------------------------------
struct Slot {
    coop::net::IntentQueue<coop::net::MicrowaveIntentPayload> queue;
    bool     waitSaid = false;
    uint64_t nextSayMs = 0;
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

void Deny(uint8_t slot, const std::wstring& key, const char* why) {
    ++g_denied;
    Slot& sl = g_slots[slot];
    const uint64_t now = NowMs();
    if (now < sl.nextSayMs) return;
    sl.nextSayMs = now + kSayMs;
    UE_LOGI("[MICROWAVE] host: DENY slot %u's press on key='%ls' -- %s", static_cast<unsigned>(slot), key.c_str(), why);
}

bool Execute(coop::net::Session& s, const coop::net::MicrowaveIntentPayload& p, uint8_t slot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* mw = coop::prop_key_index::Find(key);
    if (!mw || !MW::IsMicrowave(mw)) {
        Deny(slot, key, "no microwave holds the key");
        return true;
    }
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) {
        Deny(slot, key, "the sender's player is not alive");
        return true;
    }
    const EL::IntentSubject subject = EL::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(mw);
    if (subject.outcome == EL::IntentOutcome::NoBody) {
        if (!g_slots[slot].waitSaid) {
            g_slots[slot].waitSaid = true;
            UE_LOGI("[MICROWAVE] host: slot %u's presses wait: the host has no body for it yet",
                    static_cast<unsigned>(slot));
        }
        return false;
    }
    if (!subject) {
        Deny(slot, key, EL::OutcomeName(subject.outcome));
        return true;
    }
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    void* body = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    if (!ue_wrap::appliance::CallAction(mw, body, p.action)) {
        Deny(slot, key, "the host's copy did not take the call");
        return true;
    }
    ++g_ran;
    MarkDirty(mw);
    UE_LOGI("[MICROWAVE] host: ran slot %u's action %u on key='%ls' (#%llu)", static_cast<unsigned>(slot),
            static_cast<unsigned>(p.action), key.c_str(), static_cast<unsigned long long>(g_ran));
    return true;
}

// ---- client: the presses and the loop's effects -------------------------------------------------------------------
sg::Verdict OnActionPre(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsClient(s) || !c.object || !c.locals) return sg::Verdict::Run;
    const int32_t off = g_actionParam.Of(c.function);
    if (off < 0) return sg::Verdict::Run;
    const uint8_t action = c.locals[off];
    if (action != P::name::MicrowaveActionMode && action != P::name::MicrowaveActionStart) return sg::Verdict::Run;
    const int32_t pOff = g_playerParam.Of(c.function);
    void* player = pOff >= 0 ? *reinterpret_cast<void* const*>(c.locals + pOff) : nullptr;
    if (player && !coop::players::Registry::Get().IsLocal(player)) return sg::Verdict::Run;
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(c.object);
    bool sent = false;
    if (!key.empty() && key != L"None" && key.size() <= sizeof(coop::net::WireKey::data)) {
        coop::net::MicrowaveIntentPayload p{};
        coop::net::WireKeyFromString(key, p.key);
        p.action = action;
        sent = s->SendReliable(coop::net::ReliableKind::MicrowaveIntent, &p, sizeof(p));
    }
    if (sent) {
        ++g_sent;
        UE_LOGI("[MICROWAVE] client: sent action %u on key='%ls' (#%llu)", static_cast<unsigned>(action), key.c_str(),
                static_cast<unsigned long long>(g_sent));
        return sg::Verdict::Cancel;
    }
    UE_LOGW("[MICROWAVE] client: the press on microwave key='%ls' was not sent -- it runs here alone", key.c_str());
    if (g_localBit != 0) {
        sg::MarkScope(c, g_localBit);
        g_actionFn = c.function;
    }
    return sg::Verdict::Run;
}

void OnActionPost(const sg::Call& c) {
    if (IsHost(g_session.load(std::memory_order_acquire)) && c.object) MarkDirty(c.object);
}

bool LocalPressRunning() { return g_actionFn && g_localBit != 0 && (sg::ScopeBits(g_actionFn) & g_localBit) != 0; }

sg::Verdict OnWavePre(const sg::Call& c) {
    if (!IsClient(g_session.load(std::memory_order_acquire)) || LocalPressRunning() || !c.object)
        return sg::Verdict::Run;
    ++g_cancelledWave;
    if (!g_waveSaid) {
        g_waveSaid = true;
        UE_LOGI("[MICROWAVE] client: a microwave's wave is the host's -- cancelled here (its run arrives as "
                "MicrowaveState; said once)");
    }
    return sg::Verdict::Cancel;
}

void OnWavePost(const sg::Call& c) {
    if (IsHost(g_session.load(std::memory_order_acquire)) && c.object) MarkDirty(c.object);
}

// The host's own microwaves, noted as they tick: a run its own player started reaches every peer.
void OnTickPost(const sg::Call& c) {
    if (!IsHost(g_session.load(std::memory_order_acquire)) || !c.object) return;
    for (const HostRec& r : g_host)
        if (r.actor.Is(c.object)) return;
    Note(c.object);
}

// A loop's effect on what is inside, called from a microwave's frame on a client: the host's loop is the one that
// heats and sparks. A loop running here despite the cancelled wave (one started before the lane) is stopped at its
// effects.
sg::Verdict OnEffectPre(const sg::Call& c) {
    if (!IsClient(g_session.load(std::memory_order_acquire)) || !c.callerObject || LocalPressRunning())
        return sg::Verdict::Run;
    if (!MW::IsMicrowave(c.callerObject)) return sg::Verdict::Run;
    ++g_cancelledEffects;
    if (!g_effectSaid) {
        g_effectSaid = true;
        UE_LOGI("[MICROWAVE] client: a microwave loop's effect on what is inside is the host's -- cancelled here (said "
                "once)");
    }
    return sg::Verdict::Cancel;
}

// The plates this client turns, from the host's running state.
std::vector<ue_wrap::CachedObjRef> g_turning;
uint64_t g_lastTurnMs = 0;

void SetTurning(void* mw, bool on) {
    for (size_t i = 0; i < g_turning.size(); ++i)
        if (g_turning[i].Is(mw)) {
            if (!on) g_turning.erase(g_turning.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    if (!on || g_turning.size() >= kMaxMicrowaves) return;
    ue_wrap::CachedObjRef r;
    r.Set(mw);
    g_turning.push_back(std::move(r));
}

void TurnPlates() {
    const uint64_t now = NowMs();
    const float dt = g_lastTurnMs == 0 ? 0.f : static_cast<float>(now - g_lastTurnMs) / 1000.f;
    g_lastTurnMs = now;
    if (g_turning.empty() || dt <= 0.f || dt > 0.5f) return;
    for (size_t i = 0; i < g_turning.size();) {
        void* mw = g_turning[i].Get();
        MW::State st;
        if (!mw || !MW::ReadState(mw, st) || !st.active) {
            g_turning.erase(g_turning.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (st.powered) {
            MW::TurnPlate(mw, dt * P::name::MicrowaveSpinDegPerSec);
            MW::WriteTime(mw, st.time - dt > 0.f ? st.time - dt : 0.f);   // the look-at's count, put right each second
        }
        ++i;
    }
}

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        const Watch& w = kWatches[i];
        sg::PreFn pre = nullptr;
        sg::PostFn post = nullptr;
        switch (w.tag) {
            case kTagAction: pre = &OnActionPre; post = &OnActionPost; break;
            case kTagWave: pre = &OnWavePre; post = &OnWavePost; break;
            case kTagTick: post = &OnTickPost; break;
            default: pre = &OnEffectPre; break;
        }
        if (sg::WatchName(*w.row, w.tag, pre, post)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[MICROWAVE] the script-body gate refused the watch on %s -- that part of a microwave runs on every "
                "peer", w.what);
    }
}

void Settle() {
    if (g_settled) return;
    int live = 0, pending = 0;
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] == Reg::Pending) ++pending;
        else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kWatches[i].row, kWatches[i].tag)) ++live;
    }
    if (live == kWatchCount) {
        g_settled = true;
        UE_LOGI("[MICROWAVE] the microwave watches are live: the press, wave, tick and the loop's two effects");
    } else if (pending == 0 && sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[MICROWAVE] %d of %d microwave watches are dead -- those parts run on every peer", kWatchCount - live,
                kWatchCount);
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session) return;
    if (g_localBit == 0) g_localBit = sg::ScopeBitFor(&g_localBit);
    RegisterWatches();
}

void Tick(coop::net::Session& session) {
    if (!g_settled) {
        sg::ResolvePendingNames();
        Settle();
    }
    if (!session.running()) return;
    if (session.role() == coop::net::Role::Host) {
        for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
            Slot& sl = g_slots[slot];
            sl.queue.Serve(kBudget, NowMs, [&](const coop::net::MicrowaveIntentPayload& p) {
                if (!Execute(session, p, slot)) return coop::net::Served::Wait;
                sl.waitSaid = false;
                return coop::net::Served::Done;
            });
        }
        PumpHost(session);
    } else {
        TurnPlates();
    }
}

void OnPeerWorldReady(int slot) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s) || slot < 1 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    int sent = 0;
    for (const HostRec& r : g_host) {
        void* mw = r.actor.Get();
        MW::State st;
        coop::net::MicrowaveStatePayload p{};
        if (!mw || !MW::ReadState(mw, st) || !Fill(mw, st, false, p)) continue;
        if (s->SendReliableToSlot(slot, coop::net::ReliableKind::MicrowaveState, &p, sizeof(p))) ++sent;
    }
    if (sent) UE_LOGI("[MICROWAVE] host: slot %d's world is up -- sent %d microwave state(s)", slot, sent);
}

void OnIntent(coop::net::Session& session, const coop::net::MicrowaveIntentPayload& p, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host || senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    Slot& sl = g_slots[senderSlot];
    auto copy = p;
    if (!sl.queue.Push(std::move(copy), kMaxPending)) Deny(senderSlot, coop::net::StringFromWireKey(p.key), "queue full");
}

void OnState(const coop::net::MicrowaveStatePayload& p) {
    if (!IsClient(g_session.load(std::memory_order_acquire))) return;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* mw = coop::prop_key_index::Find(key);
    MW::State was;
    if (!mw || !MW::ReadState(mw, was)) {
        ++g_unknown;
        return;
    }
    MW::WriteMode(mw, p.timeMode, p.timer);
    const bool active = (p.flags & MS::kActive) != 0;
    if (active) {
        if (!was.active) {
            MW::ShowRunning(mw, p.remaining);
            UE_LOGI("[MICROWAVE] client: key='%ls' RUNNING, %.1f s left", key.c_str(), static_cast<double>(p.remaining));
        } else {
            MW::WriteTime(mw, p.remaining);
        }
        SetTurning(mw, true);
    } else {
        if (was.active) {
            MW::ShowStopped(mw, (p.flags & MS::kDing) != 0);
            UE_LOGI("[MICROWAVE] client: key='%ls' stopped%s", key.c_str(), (p.flags & MS::kDing) ? " (ding)" : "");
        }
        SetTurning(mw, false);
    }
    ++g_applied;
}

bool WellFormed(const coop::net::MicrowaveStatePayload& p) {
    if (p.key.len == 0 || p.key.len > sizeof(p.key.data)) return false;
    if (p.reserved[0] || p.reserved[1] || p.reserved[2] || (p.flags & ~MS::kKnown) != 0) return false;
    if (!std::isfinite(p.timer) || !std::isfinite(p.remaining)) return false;
    if (p.timer < 0.f || p.timer > 3600.f || p.remaining < 0.f || p.remaining > 3600.f) return false;
    return p.timeMode >= 0 && p.timeMode < P::name::MicrowaveModeCount;
}

void CountDropped() { ++g_dropped; }

void OnDisconnect() {
    if (g_sent || g_ran || g_denied || g_dropped || g_published || g_applied || g_cancelledWave || g_cancelledEffects)
        UE_LOGI("[MICROWAVE] session end -- presses sent=%llu run=%llu denied=%llu dropped=%llu states published=%llu "
                "applied=%llu unknown=%llu; cancelled wave=%llu effects=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_ran),
                static_cast<unsigned long long>(g_denied), static_cast<unsigned long long>(g_dropped),
                static_cast<unsigned long long>(g_published), static_cast<unsigned long long>(g_applied),
                static_cast<unsigned long long>(g_unknown), static_cast<unsigned long long>(g_cancelledWave),
                static_cast<unsigned long long>(g_cancelledEffects));
    g_host.clear();
    g_turning.clear();
    g_lastTurnMs = 0;
    g_sent = g_ran = g_denied = g_dropped = g_published = g_applied = g_cancelledWave = g_cancelledEffects = 0;
    g_unknown = 0;
    g_waveSaid = g_effectSaid = g_publishSaid = g_fullSaid = false;
}

}  // namespace coop::microwave_sync

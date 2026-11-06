// coop/props/fire_extinguisher_sync.cpp -- see coop/props/fire_extinguisher_sync.h.

#include "coop/props/fire_extinguisher_sync.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/props/crate_break.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_record_gates.h"
#include "coop/props/prop_save_data.h"

#include "ue_wrap/actors/fire_extinguisher.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coop::props::fire_extinguisher_sync {
namespace {

namespace EL = coop::element;
namespace FE = ue_wrap::fire_extinguisher;
namespace P  = ue_wrap::profile;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowShoot, "fire_extinguisher_sync", L"prop_fireExt_C", L"shoot", Declared);
UE_FACT_FN(kRowRay, "fire_extinguisher_sync", L"prop_fireExt_C", L"extinguishRay", Declared);
constexpr int kTagShoot = 0x46455331;   // 'FES1'
constexpr int kTagRay = 0x46455332;     // 'FES2'

constexpr const wchar_t* kClassName = L"prop_fireExt_C";

std::atomic<coop::net::Session*> g_session{nullptr};
coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

bool IsClientSession() {
    coop::net::Session* s = LoadSession();
    return s && s->connected() && s->role() == coop::net::Role::Client;
}

constexpr int kMaxSaid = 20;

// ---- the watches ----------------------------------------------------------------------------------------------

// The depth of this module's own replay of the host's burst on a client: the one scope `shoot` may run in there.
int g_replaying = 0;

// HOST: the extinguishers whose `shoot` has run, and when each one's record next goes out. Marked from the body's post,
// drained on the tick: the capture and the send are engine calls, which a post callback leaves to the pump.
constexpr size_t   kMaxBursts = 16;
constexpr uint64_t kRepublishMs = 1000;
struct Burst {
    ue_wrap::CachedObjRef actor;
    uint64_t              nextMs = 0;
};
std::vector<Burst> g_bursts;

uint32_t g_marked = 0;
uint32_t g_published = 0;
uint32_t g_replayed = 0;
uint32_t g_ceased = 0;
uint32_t g_cancelledShoot = 0;
uint32_t g_cancelledRay = 0;
int      g_markSaid = 0;
int      g_publishSaid = 0;
int      g_replaySaid = 0;
bool     g_fullSaid = false;
bool     g_shootCancelSaid = false;
bool     g_raySaid = false;
bool     g_noKeySaid = false;
bool     g_badRecordSaid = false;
bool     g_unresolvedSaid = false;
bool     g_intentSaid = false;

// A client's `shoot` that is not the replay: a guard of the subclass reached it from this machine's own physics, a
// hit or a fire, or a loadData. The host's burst arrives as its record.
sg::Verdict OnShootPre(const sg::Call&) {
    if (!IsClientSession() || g_replaying > 0) return sg::Verdict::Run;
    ++g_cancelledShoot;
    if (!g_shootCancelSaid) {
        g_shootCancelSaid = true;
        UE_LOGI("[FIREEXT] client: a burst this machine started is the host's -- cancelled (the host's burst arrives "
                "as its record; said once)");
    }
    return sg::Verdict::Cancel;
}

void MarkBurst(void* actor) {
    for (Burst& b : g_bursts)
        if (b.actor.Get() == actor) {
            b.nextMs = 0;
            return;
        }
    if (g_bursts.size() >= kMaxBursts) {
        if (!g_fullSaid) {
            g_fullSaid = true;
            UE_LOGW("[FIREEXT] host: %zu extinguishers already burst -- this one is not published (said once)",
                    kMaxBursts);
        }
        return;
    }
    Burst b;
    b.actor.Set(actor);
    g_bursts.push_back(std::move(b));
    ++g_marked;
    if (g_markSaid < kMaxSaid) {
        ++g_markSaid;
        const EL::ElementId eid = EL::Registry::Get().EidForActor(actor);
        UE_LOGI("[FIREEXT] host: eid=%u burst%s (#%u)", static_cast<unsigned>(eid),
                coop::props::crate_break::HostApplying() ? " from a client's hit or contact" : "", g_marked);
    }
}

void OnShootPost(const sg::Call& c) {
    coop::net::Session* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    MarkBurst(c.object);
}

// The client's loop never extinguishes: the host's does. A hand spray (`active` clear) is the player's own.
sg::Verdict OnRayPre(const sg::Call& c) {
    if (!IsClientSession()) return sg::Verdict::Run;
    bool thrusting = false;
    if (!FE::ReadThrusting(c.object, thrusting) || !thrusting) return sg::Verdict::Run;
    ++g_cancelledRay;
    if (!g_raySaid) {
        g_raySaid = true;
        UE_LOGI("[FIREEXT] client: a burst's extinguishing is the host's -- the ray here is cancelled (said once)");
    }
    return sg::Verdict::Cancel;
}

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_regShoot = Reg::Pending;
Reg  g_regRay = Reg::Pending;
bool g_settledShoot = false;
bool g_settledRay = false;

void Settle(Reg reg, bool& settled, const ue_wrap::facts::Function& row, int tag, const char* verb, const char* dead) {
    if (settled || reg == Reg::Pending) return;
    if (reg == Reg::Registered && sg::NameWatchLive(row, tag)) {
        settled = true;
        UE_LOGI("[FIREEXT] the prop_fireExt_C %s watch is live", verb);
    } else if (reg == Reg::Refused || sg::NameWatchSettled(row, tag)) {
        settled = true;
        UE_LOGE("[FIREEXT] the %s watch is dead -- %s", verb, dead);
    }
}

// ---- the record's rules ---------------------------------------------------------------------------------------

// Only the host's own burst is published: a client's record for an extinguisher is refused, whoever sent it.
bool AcceptsClientRecord(void*, uint8_t senderSlot) {
    if (!g_intentSaid) {
        g_intentSaid = true;
        UE_LOGI("[FIREEXT] host: a record for an extinguisher from slot %u is refused -- the burst is the host's (said "
                "once)", static_cast<unsigned>(senderSlot));
    }
    return false;
}

// CLIENT: the extinguishers this machine has started the host's burst on, so one record that lands twice, or a
// correction, never starts a second loop on a prop whose `active` bit the cancelled guards may have set already.
constexpr size_t kMaxReplayed = 32;
std::vector<ue_wrap::CachedObjRef> g_replayedSet;

bool IsReplayed(void* actor) {
    for (const ue_wrap::CachedObjRef& r : g_replayedSet)
        if (r.Get() == actor) return true;
    return false;
}

void SetReplayed(void* actor, bool on) {
    for (size_t i = 0; i < g_replayedSet.size(); ++i)
        if (g_replayedSet[i].Get() == actor) {
            if (!on) g_replayedSet.erase(g_replayedSet.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    if (!on) return;
    if (g_replayedSet.size() >= kMaxReplayed) g_replayedSet.erase(g_replayedSet.begin());   // the oldest burst is long spent
    ue_wrap::CachedObjRef ref;
    ref.Set(actor);
    g_replayedSet.push_back(std::move(ref));
}

// The record's own two members, validated: the group the leaf class appended, a finite charge in [0, 1].
bool ReadState(const ue_wrap::save_record::SaveRecord& rec, float& life, bool& active) {
    const size_t g = static_cast<size_t>(P::name::FireExtRecordGroup);
    if (rec.floats.size() <= g || rec.floats[g].empty() || rec.bools.size() <= g || rec.bools[g].empty()) return false;
    life = rec.floats[g][0];
    active = rec.bools[g][0] != 0;
    return std::isfinite(life) && life >= 0.f && life <= 1.f;
}

// CLIENT: land the host's record on an extinguisher: `life` and `active` only. The record's other members are the
// spawn row's and the drive stream's, and the game's loadData would re-run the prop's init and start the burst.
bool ApplyRecord(void* actor, const ue_wrap::save_record::SaveRecord& rec) {
    float life = 0.f;
    bool active = false;
    if (!ReadState(rec, life, active)) {
        if (!g_badRecordSaid) {
            g_badRecordSaid = true;
            UE_LOGW("[FIREEXT] a record for an extinguisher has no life/active pair in group %d, or one out of range -- "
                    "not applied (said once)", P::name::FireExtRecordGroup);
        }
        return false;
    }
    if (!FE::WriteCharge(actor, life)) {
        if (!g_unresolvedSaid) {
            g_unresolvedSaid = true;
            UE_LOGW("[FIREEXT] the extinguisher's life did not resolve -- the host's burst is not applied here");
        }
        return false;
    }
    if (active && life > 0.f) {
        // The edge: the burst starts once through the game's own verb, inside the scope the gate lets it run in.
        if (!IsReplayed(actor)) {
            ++g_replaying;
            const bool ran = FE::CallShoot(actor);
            --g_replaying;
            if (!ran) {
                if (!g_unresolvedSaid) {
                    g_unresolvedSaid = true;
                    UE_LOGW("[FIREEXT] the extinguisher's shoot did not run -- the host's burst is not shown here");
                }
                return false;
            }
            SetReplayed(actor, true);
            ++g_replayed;
            if (g_replaySaid < kMaxSaid) {
                ++g_replaySaid;
                UE_LOGI("[FIREEXT] client: replayed shoot on eid=%u life=%.3f (#%u)",
                        static_cast<unsigned>(EL::Registry::Get().EidForActor(actor)), static_cast<double>(life),
                        g_replayed);
            }
        }
        return true;
    }
    if (active) {
        // Spent: the flag stays set as the game leaves it, and the components stop. Marked replayed, so a late record
        // for a burst that is over never starts it.
        FE::WriteThrusting(actor, true);
        FE::StopThrust(actor);
        SetReplayed(actor, true);
        ++g_ceased;
        if (g_replaySaid < kMaxSaid) {
            ++g_replaySaid;
            UE_LOGI("[FIREEXT] client: eid=%u burst spent -- thrust stopped (#%u)",
                    static_cast<unsigned>(EL::Registry::Get().EidForActor(actor)), g_ceased);
        }
        return true;
    }
    if (IsReplayed(actor)) {
        FE::StopThrust(actor);
        SetReplayed(actor, false);
    }
    return true;
}

// ---- host: the bursts' pump -----------------------------------------------------------------------------------

void PublishBurst(coop::net::Session& s, void* actor, float life) {
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    if (key.empty()) {
        if (!g_noKeySaid) {
            g_noKeySaid = true;
            UE_LOGW("[FIREEXT] host: a bursting extinguisher has no save key -- its burst reaches no peer (said once)");
        }
        return;
    }
    // A refusal is the lane's own: no peer ready (the join's snapshot carries the record), or the send went onto its
    // retry register.
    if (!coop::prop_save_data::Publish(&s, actor, key)) return;
    ++g_published;
    if (g_publishSaid < kMaxSaid) {
        ++g_publishSaid;
        UE_LOGI("[FIREEXT] host: eid=%u record published life=%.3f key='%ls' (#%u)",
                static_cast<unsigned>(EL::Registry::Get().EidForActor(actor)), static_cast<double>(life), key.c_str(),
                g_published);
    }
}

void PumpBursts(coop::net::Session& s) {
    if (g_bursts.empty()) return;
    const uint64_t now = coop::net::NowMs();
    for (size_t i = 0; i < g_bursts.size();) {
        Burst& b = g_bursts[i];
        void* actor = b.actor.Get();
        float life = 0.f;
        bool active = false;
        if (!actor || !FE::ReadCharge(actor, life) || !FE::ReadThrusting(actor, active)) {
            g_bursts.erase(g_bursts.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (active && life > 0.f) {
            // The rocketing prop is the host's to stream while it thrusts; a prop in a hand is refused by the claim.
            coop::prop_drive_host::Claim(actor, "a fire extinguisher's burst");
            if (now >= b.nextMs) {
                b.nextMs = now + kRepublishMs;
                PublishBurst(s, actor, life);
            }
            ++i;
            continue;
        }
        // Spent: the last record goes, and the prop coasts to rest.
        PublishBurst(s, actor, life);
        coop::prop_drive_host::Release(actor);
        g_bursts.erase(g_bursts.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session) return;
    // Both roles: a process can host one session and join the next; the callbacks ask the role.
    if (g_regShoot == Reg::Pending)
        g_regShoot = sg::WatchName(kRowShoot, kTagShoot, &OnShootPre, &OnShootPost) ? Reg::Registered : Reg::Refused;
    if (g_regRay == Reg::Pending)
        g_regRay = sg::WatchName(kRowRay, kTagRay, &OnRayPre, nullptr) ? Reg::Registered : Reg::Refused;
    coop::prop_record_gates::RegisterIntentGate(kClassName, &AcceptsClientRecord);
    coop::prop_record_gates::RegisterApplier(kClassName, &ApplyRecord);
}

void Tick() {
    // The class names convert through a dispatch: this is a top-level pump pass.
    FE::ResolveNames();
    if ((!g_settledShoot && g_regShoot == Reg::Registered) || (!g_settledRay && g_regRay == Reg::Registered))
        sg::ResolvePendingNames();
    Settle(g_regShoot, g_settledShoot, kRowShoot, kTagShoot, "shoot",
           "a client's own burst runs on that client alone and the host's reaches no peer");
    Settle(g_regRay, g_settledRay, kRowRay, kTagRay, "extinguishRay",
           "a client's replayed burst also extinguishes on that client");
    coop::net::Session* s = LoadSession();
    if (s && s->running() && s->role() == coop::net::Role::Host) PumpBursts(*s);
}

void OnDisconnect() {
    if (g_marked != 0 || g_replayed != 0 || g_cancelledShoot != 0)
        UE_LOGI("[FIREEXT] this session: %u bursts marked and %u records published here, %u replayed and %u spent "
                "bursts landed, %u shoot and %u ray calls cancelled", g_marked, g_published, g_replayed, g_ceased,
                g_cancelledShoot, g_cancelledRay);
    g_bursts.clear();
    g_replayedSet.clear();
    g_replaying = 0;
    g_marked = g_published = g_replayed = g_ceased = g_cancelledShoot = g_cancelledRay = 0;
    g_markSaid = g_publishSaid = g_replaySaid = 0;
    g_fullSaid = g_shootCancelSaid = g_raySaid = g_noKeySaid = g_badRecordSaid = g_unresolvedSaid = g_intentSaid = false;
}

}  // namespace coop::props::fire_extinguisher_sync

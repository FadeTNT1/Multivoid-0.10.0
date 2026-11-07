// coop/props/crate_break.cpp -- see coop/props/crate_break.h.

#include "coop/props/crate_break.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/intent_queue.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/throw_bounds.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_syncer.h"
#include "coop/props/prop_throw.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/breakable_explosive.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/cubicle.h"
#include "ue_wrap/actors/fire_extinguisher.h"
#include "ue_wrap/actors/int_objects.h"
#include "ue_wrap/actors/physics_impact.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_flags.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/obj_keyed_cache.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/asologo_piece.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace coop::props::crate_break {
namespace {

namespace EL = coop::element;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using coop::net::Served;
using ue_wrap::FVector;

// The container's bodies a break's spill can run inside: its destroy, and the two breaks the component's own break
// calls (customBreak). Read from the pak's name map of prop_container; which of them spills is not yet measured.
UE_FACT_FN(kRowDestroyed, "crate_break", L"prop_container_C", L"ReceiveDestroyed", Declared);
UE_FACT_FN(kRowBroken, "crate_break", L"prop_container_C", L"broken", Declared);
UE_FACT_FN(kRowBrokenFire, "crate_break", L"prop_container_C", L"broken_fire", Declared);
constexpr int kRows = 3;
const ue_wrap::facts::Function* const kRowOf[kRows] = {&kRowDestroyed, &kRowBroken, &kRowBrokenFire};
constexpr int kTag[kRows] = {0x43425231, 0x43425232, 0x43425233};   // 'CBR1', 'CBR2', 'CBR3'
const char* const kVerb[kRows] = {"ReceiveDestroyed", "broken", "broken_fire"};

std::atomic<coop::net::Session*> g_session{nullptr};
coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

bool IsClientSession() {
    coop::net::Session* s = LoadSession();
    return s && s->connected() && s->role() == coop::net::Role::Client;
}

// Each side's first kMaxSaid lines of a kind are said, the rest are counted.
constexpr int kMaxSaid = 20;

float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool Finite(const FVector& v) { return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z); }

// The slack past a prop's colliding bounds a struck point may lie, cm: a hit's trace point and the host's copy of a
// static prop agree to the surface, and a moving prop trails by the stream's latency.
constexpr float kBoundsSlackCm = 50.f;

// Does `p` lie on `prop`: within `radius` of its origin (a crate or a barrel), or inside its colliding bounds grown by
// the slack (a cubicle partition or a vending machine, wider than any radius from its origin). False for a NaN.
bool PointOnProp(void* prop, const FVector& p, const FVector& origin, float radius) {
    if (Dist(p, origin) <= radius) return true;
    FVector c{}, e{};
    if (!ue_wrap::engine::GetActorBounds(prop, /*onlyColliding=*/true, c, e) || !Finite(c) || !Finite(e)) return false;
    return std::fabs(p.X - c.X) <= e.X + kBoundsSlackCm && std::fabs(p.Y - c.Y) <= e.Y + kBoundsSlackCm &&
           std::fabs(p.Z - c.Z) <= e.Z + kBoundsSlackCm;
}

// A parameter's storage in the call: the caller's, for an out or reference parameter, else the frame's own slot.
const uint8_t* ParamBytes(const sg::Call& c, int32_t offset) {
    if (offset < 0 || !c.locals) return nullptr;
    const uint8_t* out = sg::OutParamPtr(c, offset);
    return out ? out : c.locals + offset;
}

// ---- client: a contact on a container's mirror ----------------------------------------------------------------

R::ParamOffset g_damageOff{L"damage"};
R::ParamOffset g_impactOff{L"impact"};
R::ParamOffset g_hitOff{L"hit"};
R::ParamOffset g_actorOff{L"actor"};

// One contact can reach impactDamage twice (the struck body's hit and the hitter's): a container sends once in this.
constexpr uint64_t kSameContactMs = 80;
// A causer that has no element id yet (a throw the host has not bound) is waited for this long.
constexpr uint64_t kCauserWaitMs = 1500;
constexpr size_t   kMaxWaiting = 8;

struct Waiting {
    ue_wrap::CachedObjRef               causer;
    coop::net::PropContactIntentPayload p{};
    uint64_t                            untilMs = 0;
};
std::vector<Waiting> g_waiting;

struct LastSent {
    uint32_t eid = 0;
    uint64_t ms = 0;
};
constexpr size_t kLastSent = 8;
LastSent g_lastSent[kLastSent];
size_t   g_lastNext = 0;
uint16_t g_nextContact = 0;

int  g_contactSaid = 0;
bool g_paramsSaid = false;
bool g_unboundSaid = false;
bool g_waitFullSaid = false;
bool g_noCauserSaid = false;
ue_wrap::ObjKeyedCache<bool, 24> g_foreignSaid;   // the causer classes whose first foreign impact has been said

// The host can check this client's flight of `prop`: a throw or a swing this client minted, or a row it syncs. A prop
// only held in a hand streams no row, so a carried prop's bumps are not sent.
bool FliesHere(void* prop) {
    return coop::prop_throw::SeqFor(prop) != 0 || coop::prop_syncer::AuthorsActorHere(prop);
}

bool RecentlySent(uint32_t eid, uint64_t now) {
    for (const LastSent& l : g_lastSent)
        if (l.eid == eid && now - l.ms < kSameContactMs) return true;
    return false;
}

// Send the contact unless its other half has just gone; true when it went or was that other half.
bool SendContact(coop::net::Session& s, coop::net::PropContactIntentPayload& p) {
    const uint64_t now = coop::net::NowMs();
    if (RecentlySent(p.targetEid, now)) return true;
    if (++g_nextContact == 0) ++g_nextContact;
    p.contactId = g_nextContact;
    if (!s.SendReliableToSlot(0, coop::net::ReliableKind::PropContactIntent, &p, sizeof(p))) {
        UE_LOGW("[CRATE] client: the contact on container eid=%u was not sent", static_cast<unsigned>(p.targetEid));
        return false;
    }
    g_lastSent[g_lastNext] = LastSent{p.targetEid, now};
    g_lastNext = (g_lastNext + 1) % kLastSent;
    if (g_contactSaid < kMaxSaid) {
        ++g_contactSaid;
        UE_LOGI("[CRATE] client: contact on container eid=%u causer=%u dmg=%.1f speed=%.0f id=%u sent to the host",
                static_cast<unsigned>(p.targetEid), static_cast<unsigned>(p.causerEid), static_cast<double>(p.damage),
                static_cast<double>(p.speed), static_cast<unsigned>(p.contactId));
    }
    return true;
}

void ServeWaiting(coop::net::Session& s) {
    if (g_waiting.empty()) return;
    const uint64_t now = coop::net::NowMs();
    for (size_t i = 0; i < g_waiting.size();) {
        Waiting& w = g_waiting[i];
        void* causer = w.causer.Get();
        const EL::ElementId eid = causer ? EL::Registry::Get().EidForActor(causer) : EL::kInvalidId;
        if (eid != EL::kInvalidId) {
            w.p.causerEid = eid;
            SendContact(s, w.p);
        } else if (causer && now < w.untilMs) {
            ++i;
            continue;
        } else {
            UE_LOGI("[CRATE] client: a contact on container eid=%u is lost -- its causer %s",
                    static_cast<unsigned>(w.p.targetEid), causer ? "never bound to an element" : "is gone");
        }
        g_waiting.erase(g_waiting.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// ---- client: a spill of a copy the host destroyed -------------------------------------------------------------

constexpr int kMaxSpillFns = 8;   // the watched bodies, one per class that runs them
void* g_spillFns[kMaxSpillFns] = {};
int   g_spillFnCount = 0;
std::vector<ue_wrap::CachedObjRef> g_dissolve;
constexpr size_t kMaxDissolve = 64;
uint32_t g_dissolved = 0;
int      g_spillSaid = 0;

sg::Verdict OnSpillBodyPre(const sg::Call& c) {
    if (!IsClientSession()) return sg::Verdict::Run;
    // The body's function is what InsideMirrorSpill asks the gate about; a class that overrides it has its own.
    for (int i = 0; i < g_spillFnCount; ++i)
        if (g_spillFns[i] == c.function) return sg::Verdict::Run;
    if (g_spillFnCount < kMaxSpillFns) g_spillFns[g_spillFnCount++] = c.function;
    return sg::Verdict::Run;
}

// ---- host: the contact's judge --------------------------------------------------------------------------------

// A sender's contacts run in arrival order from a bounded queue at a bounded rate (hit_intent's shape).
constexpr coop::net::IntentBudget kBudget{8.0f, 4.0f};
constexpr size_t kMaxPending = 8;
// A container is a crate or a barrel, wider than a swing's prop (hit_target_prop's 100): the point lies on it.
constexpr float kContainerRadiusCm = 200.f;
// The host's copy of a thrown causer trails the thrower's by the stream's latency, at flight speed.
constexpr float kCauserSlackCm = 1000.f;
// A ceiling, not a recomputation: the native impact's damage is computed on the thrower's machine and not read here.
constexpr float kMaxContactDamage = 500.f;
constexpr float kMaxContactSpeedCmS = coop::net::throw_bounds::kMaxFlightSpeedCmS;
// One contact's two halves from one sender, if both came through.
constexpr uint64_t kMinContactGapMs = 80;
// The causer's election can land after the contact: the release, the drop intent's birth.
constexpr uint64_t kSyncWaitMs = 1500;

struct Queued {
    coop::net::PropContactIntentPayload req{};
    uint64_t                            arrivedMs = 0;
};

struct Reason {
    const char* why = nullptr;
    uint32_t    count = 0;
};
constexpr int kMaxReasons = 16;
constexpr int kSeen = 16;

struct Slot {
    coop::net::IntentQueue<Queued> queue;
    Reason   reasons[kMaxReasons];
    uint16_t seen[kSeen] = {};
    int      seenNext = 0;
    uint32_t lastTarget = 0;
    uint64_t lastMs = 0;
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

int      g_applying = 0;
uint32_t g_swingsApplied = 0;
uint32_t g_contactsApplied = 0;
int      g_swingSaid = 0;
int      g_pieceHitSaid = 0;
int      g_appliedSaid = 0;

void Refuse(int slot, const char* why) {
    if (slot < 1 || slot >= coop::players::kMaxPeers) return;
    Slot& sl = g_slots[slot];
    for (Reason& r : sl.reasons) {
        if (r.why && std::strcmp(r.why, why) != 0) continue;
        if (!r.why) {
            r.why = why;
            UE_LOGI("[CRATE] host: slot=%d contact refused: %s", slot, why);
        }
        ++r.count;
        return;
    }
}

bool Seen(const Slot& sl, uint16_t id) {
    for (uint16_t s : sl.seen)
        if (s == id) return true;
    return false;
}

void Remember(Slot& sl, uint16_t id) {
    sl.seen[sl.seenNext] = id;
    sl.seenNext = (sl.seenNext + 1) % kSeen;
}

Served Serve(coop::net::Session& s, int slot, const Queued& q) {
    const coop::net::PropContactIntentPayload& p = q.req;
    Slot& sl = g_slots[slot];
    const auto done = [&](const char* why) {
        Refuse(slot, why);
        Remember(sl, p.contactId);
        return Served::Done;
    };
    if (!s.IsSlotWorldReady(slot)) return done("slot not world-ready");
    if (Seen(sl, p.contactId)) return done("a repeated contact");
    void* target = EL::LivePropActor(static_cast<EL::ElementId>(p.targetEid));
    if (!target || !IsHostDamaged(target)) return done("not a host-damaged prop");
    // The sender's throw: the row the host elected it for stands on the causer (or on the container it threw).
    const uint32_t synced = p.causerEid != 0 ? p.causerEid : p.targetEid;
    if (coop::prop_drive_host::ClientSyncerOf(synced) != static_cast<uint8_t>(slot)) {
        if (coop::net::NowMs() - q.arrivedMs < kSyncWaitMs) return Served::Wait;
        return done(p.causerEid != 0 ? "the causer is not the sender's" : "the target is not the sender's");
    }
    void* causer = p.causerEid != 0 ? EL::LivePropActor(static_cast<EL::ElementId>(p.causerEid)) : nullptr;
    if (p.causerEid != 0 && !causer) return done("the causer is gone");

    const FVector hit{p.hitLoc[0], p.hitLoc[1], p.hitLoc[2]};
    FVector at{};
    if (!ue_wrap::engine::TryGetActorLocation(target, at)) return done("container unreadable");
    if (!PointOnProp(target, hit, at, kContainerRadiusCm)) return done("the contact is off the container");
    if (causer) {
        FVector from{};
        if (!ue_wrap::engine::TryGetActorLocation(causer, from)) return done("causer unreadable");
        if (!(Dist(hit, from) <= kCauserSlackCm)) return done("the causer is far from the contact");
    }
    if (!(p.damage >= 0.f && p.damage <= kMaxContactDamage)) return done("damage out of bounds");
    if (!(p.speed >= 0.f && p.speed <= kMaxContactSpeedCmS)) return done("speed out of bounds");
    const uint64_t now = coop::net::NowMs();
    if (sl.lastTarget == p.targetEid && now - sl.lastMs < kMinContactGapMs) return done("a contact too soon");
    sl.lastTarget = p.targetEid;
    sl.lastMs = now;

    // The component's own entry, as the thrower's impact called it: its health, its cues, its break and spill.
    const FVector impact{p.impact[0], p.impact[1], p.impact[2]};
    ++g_applying;
    const bool ran = ue_wrap::physics_impact::InvokeImpactDamage(target, p.damage, impact, hit, causer);
    --g_applying;
    if (!ran) return done("impactDamage did not run");
    Remember(sl, p.contactId);
    ++g_contactsApplied;
    if (g_appliedSaid < kMaxSaid) {
        ++g_appliedSaid;
        UE_LOGI("[CRATE] host: slot=%d contact on container eid=%u causer=%u dmg=%.1f speed=%.0f applied (#%u)", slot,
                static_cast<unsigned>(p.targetEid), static_cast<unsigned>(p.causerEid), static_cast<double>(p.damage),
                static_cast<double>(p.speed), g_contactsApplied);
    }
    return Served::Done;
}

void ServeQueues(coop::net::Session& s) {
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot)
        g_slots[slot].queue.Serve(slot, kBudget, coop::net::NowMs, [&](const Queued& q) { return Serve(s, slot, q); });
}

// ---- the watches' settle --------------------------------------------------------------------------------------

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kRows] = {Reg::Pending, Reg::Pending, Reg::Pending};
bool g_settled[kRows] = {};

void SettleWatches() {
    bool resolved = false;
    for (int i = 0; i < kRows; ++i) {
        if (g_settled[i] || g_reg[i] == Reg::Pending) continue;
        if (g_reg[i] == Reg::Registered && !resolved) {
            sg::ResolvePendingNames();
            resolved = true;
        }
        if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kRowOf[i], kTag[i])) {
            g_settled[i] = true;
            UE_LOGI("[CRATE] the prop_container_C %s watch is live", kVerb[i]);
        } else if (g_reg[i] == Reg::Refused || sg::NameWatchSettled(*kRowOf[i], kTag[i])) {
            g_settled[i] = true;
            UE_LOGW("[CRATE] the prop_container_C %s watch is dead -- a spill inside it on a client is not dissolved",
                    kVerb[i]);
        }
    }
}

}  // namespace

bool IsHostDamaged(void* prop) {
    // A fire extinguisher is host-damaged for the same reason a container is: its burst is a game-side effect (a
    // thrust, a spray, a drained charge) that one peer decides and the record then carries (fire_extinguisher_sync).
    // A breakable explosive (the vending machine, a gas can, a bomb, the explosive crystal or drive) is one because its
    // break spawns the shared world's explosion and, for the vending machine, spills and lights its stock: a client's
    // copy that broke would blast and spill a second set, or, with its explosion birth dissolved and no request for a
    // source of that kind, vanish with no blast anywhere. The host's break and its blast travel the ordinary lanes.
    // A cubicle partition is one so that a client's swing breaks it at all (a level partition has no syncer for a swing
    // to elect) and its hit and break cues reach every peer from the one copy that runs them.
    // An asologo piece (the basement's secret wall, a radiator, a security TV) is one because its health is its own and
    // its breakingSequence frees the body from Static: a client's copy that took the swing broke alone, streamed a
    // free panel the host's Static copy refused to follow, and left the host a wall nobody could see through. The
    // host's copy takes the damage, and its break reaches the clients as the pried piece's does.
    return prop && (ue_wrap::container_inventory::IsContainer(prop) || ue_wrap::fire_extinguisher::IsExtinguisher(prop) ||
                    ue_wrap::breakable_explosive::IsBreakableExplosive(prop) || ue_wrap::cubicle::IsCubicle(prop) ||
                    ue_wrap::asologo_piece::IsPiece(prop));
}

void OnClientImpact(const sg::Call& c, void* container) {
    coop::net::Session* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return;
    const uint8_t* d = ParamBytes(c, g_damageOff.Of(c.function));
    const uint8_t* im = ParamBytes(c, g_impactOff.Of(c.function));
    const uint8_t* a = ParamBytes(c, g_actorOff.Of(c.function));
    if (!d || !im) {
        if (!g_paramsSaid) {
            g_paramsSaid = true;
            UE_LOGW("[CRATE] client: impactDamage's parameters did not resolve -- a contact on a container is lost");
        }
        return;
    }
    void* causer = nullptr;
    if (a) std::memcpy(&causer, a, sizeof(causer));
    if (causer && !R::IsLive(causer)) causer = nullptr;

    // Whose contact: a prop this client throws or syncs struck the container, or the container this client threw
    // struck something. Any other impact is the host's own copy's.
    const bool byCauser = causer && causer != container && ue_wrap::prop::IsDescendantOfProp(causer) &&
                          FliesHere(causer);
    const bool bySelf = !byCauser && FliesHere(container);
    if (!byCauser && !bySelf) {
        if (!causer) {
            if (!g_noCauserSaid) {
                g_noCauserSaid = true;
                UE_LOGI("[CRATE] client: an impact on container %ls names no actor -- not this client's (said once)",
                        R::ClassNameOf(container).c_str());
            }
        } else if (void* cls = R::ClassOf(causer); cls && !g_foreignSaid.Find(cls)) {
            g_foreignSaid.Insert(cls, true);
            UE_LOGI("[CRATE] client: an impact on container %ls from %ls is not this client's -- the host's copy "
                    "takes its own (said once per class)", R::ClassNameOf(container).c_str(),
                    R::ClassNameOf(causer).c_str());
        }
        return;
    }
    const EL::ElementId target = EL::Registry::Get().EidForActor(container);
    if (target == EL::kInvalidId) {
        if (!g_unboundSaid) {
            g_unboundSaid = true;
            UE_LOGI("[CRATE] client: a contact on a container not yet bound to the host's is lost (said once)");
        }
        return;
    }

    coop::net::PropContactIntentPayload p{};
    p.targetEid = target;
    std::memcpy(&p.damage, d, sizeof(p.damage));
    std::memcpy(p.impact, im, sizeof(p.impact));
    // The point: the call's hit, else the causer's place, else the container's.
    FVector at{};
    bool haveAt = false;
    const int32_t hitSize = R::FindParamSize(c.function, L"hit");
    const uint8_t* h = ParamBytes(c, g_hitOff.Of(c.function));
    ue_wrap::hit_result::Hit hit;
    if (h && hitSize > 0 && ue_wrap::hit_result::Parse(h, hitSize, &hit) && Finite(hit.location) &&
        (hit.location.X != 0.f || hit.location.Y != 0.f || hit.location.Z != 0.f)) {
        at = hit.location;
        haveAt = true;
    }
    if (!haveAt && byCauser) haveAt = ue_wrap::engine::TryGetActorLocation(causer, at);
    if (!haveAt) ue_wrap::engine::TryGetActorLocation(container, at);
    p.hitLoc[0] = at.X;
    p.hitLoc[1] = at.Y;
    p.hitLoc[2] = at.Z;
    FVector lin{}, ang{};
    if (ue_wrap::engine::GetActorRootPhysicsVelocity(byCauser ? causer : container, lin, ang))
        p.speed = std::sqrt(lin.X * lin.X + lin.Y * lin.Y + lin.Z * lin.Z);

    if (bySelf) {
        p.causerEid = 0;
        SendContact(*s, p);
        return;
    }
    const EL::ElementId src = EL::Registry::Get().EidForActor(causer);
    if (src != EL::kInvalidId) {
        p.causerEid = src;
        SendContact(*s, p);
        return;
    }
    if (g_waiting.size() >= kMaxWaiting) {
        if (!g_waitFullSaid) {
            g_waitFullSaid = true;
            UE_LOGW("[CRATE] client: %zu contacts already wait for their causer -- this one is lost (said once)",
                    kMaxWaiting);
        }
        return;
    }
    Waiting w;
    w.causer.Set(causer);
    w.p = p;
    w.untilMs = coop::net::NowMs() + kCauserWaitMs;
    g_waiting.push_back(std::move(w));
}

bool HostApplySwing(uint32_t eid, uint8_t slot, const coop::net::HitIntentPayload& judged, const char*& why) {
    why = "the container is gone";
    void* prop = EL::LivePropActor(static_cast<EL::ElementId>(eid));
    if (!prop || !IsHostDamaged(prop)) return false;
    FVector at{};
    if (!ue_wrap::engine::TryGetActorLocation(prop, at)) return false;
    const FVector hitLoc{judged.hitLoc[0], judged.hitLoc[1], judged.hitLoc[2]};
    if (!PointOnProp(prop, hitLoc, at, kContainerRadiusCm)) {
        why = "the struck point is off the container";
        return false;
    }
    // The swinger's puppet stands in for the swinger, as the client's call named its own player.
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    void* damager = rp ? rp->GetActor() : nullptr;
    if (damager && !R::IsLive(damager)) damager = nullptr;
    const FVector impact{judged.impact[0], judged.impact[1], judged.impact[2]};
    const bool piece = ue_wrap::asologo_piece::IsPiece(prop);
    const bool staticBefore = piece && ue_wrap::prop::IsStatic(prop);
    ++g_applying;
    const bool ran = ue_wrap::int_objects::InvokeAddDamageOn(prop, ue_wrap::prop::GetStaticMesh(prop), damager,
                                                             judged.damage, hitLoc, impact);
    --g_applying;
    if (piece && g_pieceHitSaid < kMaxSaid * 2) {
        ++g_pieceHitSaid;
        // A piece's break is a latent sequence: Static may still be set when the call returns, and clears when the
        // constraints go. The host's [PIECE-BREAK] line and the clients' "static released" line follow it.
        UE_LOGI("[PIECE-HIT] host: slot=%u swing at %ls eid=%u (row '%ls') dmg=%.1f -- addDamage %s on the host's "
                "copy, static before=%d after=%d", static_cast<unsigned>(slot), R::ClassNameOf(prop).c_str(),
                static_cast<unsigned>(eid), ue_wrap::prop::GetPropNameString(prop).c_str(),
                static_cast<double>(judged.damage), ran ? "ran" : "DID NOT RUN", staticBefore ? 1 : 0,
                ue_wrap::prop::IsStatic(prop) ? 1 : 0);
    }
    if (!ran) {
        why = "addDamage did not run";
        return false;
    }
    ++g_swingsApplied;
    if (g_swingSaid < kMaxSaid) {
        ++g_swingSaid;
        UE_LOGI("[CRATE] host: slot=%u swing at container eid=%u dmg=%.1f applied on the host's copy (damager %s) (#%u)",
                static_cast<unsigned>(slot), static_cast<unsigned>(eid), static_cast<double>(judged.damage),
                damager ? "puppet" : "none", g_swingsApplied);
    }
    return true;
}

void OnContactIntent(int senderSlot, const coop::net::PropContactIntentPayload& p) {
    coop::net::Session* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGW("[CRATE] PropContactIntent received on a non-host -- dropping");
        return;
    }
    if (senderSlot < 1 || senderSlot >= coop::players::kMaxPeers) return;
    if (g_slots[senderSlot].queue.size() >= kMaxPending) {
        Refuse(senderSlot, "queue full");
        return;
    }
    Queued q;
    q.req = p;
    q.arrivedMs = coop::net::NowMs();
    g_slots[senderSlot].queue.Push(std::move(q), kMaxPending);
}

bool HostApplying() { return g_applying > 0; }

bool InsideMirrorSpill() {
    if (!IsClientSession()) return false;
    for (int i = 0; i < g_spillFnCount; ++i)
        if (sg::IsBodyActive(g_spillFns[i])) return true;
    return false;
}

void DissolveSpill(void* actor) {
    if (!actor || g_dissolve.size() >= kMaxDissolve) return;
    ue_wrap::CachedObjRef ref;
    ref.Set(actor);
    g_dissolve.push_back(ref);
    if (g_spillSaid < kMaxSaid) {
        ++g_spillSaid;
        UE_LOGI("[CRATE] client: %ls born inside a container's destroy here is a second spill -- dissolved (the "
                "host's spill arrives as its births)", R::ClassNameOf(actor).c_str());
    }
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session) return;
    // Both roles: a process can host one session and join the next; the callback asks the role.
    for (int i = 0; i < kRows; ++i)
        if (g_reg[i] == Reg::Pending)
            g_reg[i] = sg::WatchName(*kRowOf[i], kTag[i], &OnSpillBodyPre, nullptr) ? Reg::Registered : Reg::Refused;
}

void Tick() {
    SettleWatches();
    ue_wrap::cubicle::ResolveNames();
    coop::net::Session* s = LoadSession();
    if (!s) return;
    if (s->connected() && s->role() == coop::net::Role::Client) {
        ServeWaiting(*s);
        if (!g_dissolve.empty()) {
            for (ue_wrap::CachedObjRef& ref : g_dissolve)
                if (void* actor = ref.Get()) {
                    coop::remote_prop::ConsumeLocalActor(actor);
                    ++g_dissolved;
                }
            g_dissolve.clear();
        }
    }
    if (s->running() && s->role() == coop::net::Role::Host) ServeQueues(*s);
}

void OnDisconnect() {
    if (g_swingsApplied != 0 || g_contactsApplied != 0 || g_dissolved != 0)
        UE_LOGI("[CRATE] this session: %u swings and %u contacts on containers run by the host, %u spilled props "
                "dissolved here", g_swingsApplied, g_contactsApplied, g_dissolved);
    g_waiting.clear();
    g_dissolve.clear();
    for (LastSent& l : g_lastSent) l = LastSent{};
    g_lastNext = 0;
    g_spillFnCount = 0;
    g_swingsApplied = 0;
    g_contactsApplied = 0;
    g_dissolved = 0;
    g_contactSaid = 0;
    g_swingSaid = 0;
    g_pieceHitSaid = 0;
    g_appliedSaid = 0;
    g_spillSaid = 0;
    g_paramsSaid = false;
    g_unboundSaid = false;
    g_waitFullSaid = false;
    g_noCauserSaid = false;
    g_foreignSaid.Clear();
}

}  // namespace coop::props::crate_break

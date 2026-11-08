// coop/world/event_active_sync.cpp -- see coop/world/event_active_sync.h.
//
// Bytecode ground truth, from the blueprint:
//   - lib_C::setEvent(isEventActive, deactivateAmbientTrack, __WorldContext) -- the context
//     is the LAST parameter. Active adds 1 to gamemode.activeEvents and appends the context
//     to activeEvents_senders; inactive subtracts 1, removes the context, and clamps a
//     negative count back to 0. The sender registers ITSELF, so ClassOf(sender) names the
//     event class.
//   - lib_C::getEvent is `activeEvents > 0` OR the player camera being outside the base box.
//     The native no-save-during-event gate reads it; single player does not even pause
//     mid-event.
//   - Around ninety-five classes call setEvent: creature controllers, story events, pranks,
//     ambience. Several can be active at once, which is why the game keeps a refcount rather
//     than a bool.
//   - npc_arirGunStealer_C (0.10.0 capture): BeginPlay calls setEvent(true), ReceiveDestroyed the false. It ends only
//     by destroying the gun it walks to (getObjectFromKey "arirgun") and then itself; with no gun it re-reads the key
//     and walks at player 0 forever, so its count never drops (the target watchdog below).

#include "coop/world/event_active_sync.h"

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/world/event_fire_sync.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"        // P::name, the creature and target classes of the watchdog
#include "ue_wrap/engine/engine_pawn.h"      // DestroyActor, the watchdog's end of a stranded creature
#include "ue_wrap/engine/world_identity.h"   // Generation, the world the begin times belong to
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/active_events.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coop::event_active_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace AE = ue_wrap::active_events;
namespace sg = ue_wrap::script_gate;
namespace OI = ue_wrap::object_index;
namespace P  = ue_wrap::profile;
namespace E  = ue_wrap::engine;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- the host's watch on setEvent (game thread) ----------------------------------------------
constexpr int kTagEventActive = 0x45564143;  // 'EVAC'
UE_FACT_FN(kRowSetEvent, "event_active_sync", nullptr, L"setEvent", AnyClass);
bool g_watchInstalled = false;
bool g_watchLive = false;
void* g_setEventFn = nullptr;      // lib_C::setEvent, resolved from its class on first need
int32_t g_offActive = -1;          // isEventActive
int32_t g_offContext = -1;         // __WorldContext, the sender

// When each event the watch saw begin began, for the snapshot's elapsed time and the END line.
struct Began {
    int32_t objIdx;         // the sender's slot (recycled-slot-safe liveness)
    std::string className;  // ClassOf(sender), narrowed: the event's implementation class
    long long ms;           // steady-clock ms at its setEvent(true)
};
std::unordered_map<void*, Began> g_began;
uint32_t g_beganGen = 0;           // the world the begin times belong to

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---- the class->row map ------------------------------------------------------------------
// ClassOf(sender) is the event's IMPLEMENTATION class, not the list_events row the replay
// policy is keyed by. A WRONG entry replays the wrong event, and a MISSING one logs LOUD on
// the receiver and names the class to add, so a row is added only once its link has been read
// out of the runEvent case table or seen live -- never guessed. Classes that register but map to no scheduled row (sub-chains like
// trigger_alarm_C, weather senders, creature controllers spawned BY events) stay unmapped by
// design: their state rides their own lanes, not replay.
struct ClassRow { const char* className; const char* rowName; };
const ClassRow kClassRowMap[] = {
    { "obelisk_C", "obelisk" },                         // proven live
    { "piramid2_C", "piramid" },                        // proven live
    { "trigger_solarBoom_C", "solar" },
    { "trigger_vehtp_C", "vehtp" },
    { "trigger_agrav_C", "agrav" },
    { "trigger_bigmRoar_C", "call0" },
    { "trigger_wispSwarm_C", "wisps" },
    { "trigger_spawnFollowingArir_C", "arirFollower" },
    { "trigger_arirEgg_C", "arirEgg" },
    { "trigger_bedEvent_C", "bedEvent" },
    { "tentacleBallsFollower_C", "tentacleBalls" },
    { "soltomiaCleaning_C", "soltoClean" },
    { "morningUfo_C", "morningGay" },
    { "rozitBorg_C", "borgRozital" },
    { "event_bottomHoleController_C", "rozitalHole" },
    { "ventCrawler_C", "ventCrawler" },
    { "kocker_C", "ventKnocker" },
    { "grayEventController_C", "graysforest" },
    { "arirBusterSpawner_C", "arirBuster" },
    { "saltpile_C", "salt" },
    { "superEgger_C", "eggvasion" },
    { "boarInvasion_C", "boarwar" },
    { "dreamer_dreambase_C", "dreambase" },
    // The arirShip row's sender is the level-placed apparition: the eventer's runTrigger(0) reaches its appear,
    // which calls setEvent(dir) on itself (arirShipAppear_C::appear in the 0.10.0 capture). arirShip_C is the
    // alien dropper's ship and never calls setEvent.
    { "arirShipAppear_C", "arirShip" },
};

const char* RowForClass(const std::string& className) {
    for (const auto& e : kClassRowMap)
        if (className == e.className) return e.rowName;
    return nullptr;
}

// Registrants whose state a dedicated LANE snapshots at the same join edge. An EventSnapshot
// for them would only ship an unmapped-row WARN to every joiner, so they are skipped with an
// INFO and the WARN stays meaningful for genuinely uncovered classes.
struct LaneOwned { const char* className; const char* lane; };
const LaneOwned kLaneOwnedClasses[] = {
    { "trigger_alarm_C", "alarm_sync" },
    { "badSun_C", "weather_badsun" },
    { "superFog_C", "weather_fog" },        // its presence and ramp ride WeatherState (every pulse and the seed)
    { "blackFog_C", "weather_blackfog" },   // its presence and `a` ride WeatherState (every pulse and the seed)
    { "newsky_C", "sky_sync" },             // earthTp's tp: the black-hole sky rides SkyState (its seed included)
    { "noiser_C", "event_fire" },           // alienSounds: a per-viewer replay of the special; a joiner misses the cue
    { "outsideChurch_C", "church_sync" },   // 0.10.0: level-placed, host-run; its state rides ChurchState
    // The gamemode's ball UFO: the mirror's own BeginPlay registers its setEvent on a client, and a joiner's
    // mirror comes from the world-actor connect snapshot, so a replay here would only be a second begin.
    { "ufo_ballfo_C", "world_actor_sync" },
    { "ufo_boofo_spawn1_C", "world_actor_sync" },   // the boofo, joel and pillfo likewise
    { "ufo_joel_C", "world_actor_sync" },
    { "ufo_pillfo_C", "world_actor_sync" },
    { "midasufotest_C", "event_scene" },            // the ufo_midas flight: the host's phase switches a client's on
    { "arirShip_tower_C", "event_scene" },          // the radio tower's ship: the host's appear is a phase each client replays
    // The eventer's droppers (fallbody, fallcar, graystank): ufoDropper_C's box overlap calls setEvent(true) on
    // itself, so ClassOf names the subclass; each is a world-actor mirror the connect snapshot re-spawns.
    { "ufoDropper_body_C", "world_actor_sync" },
    { "ufoDropper_car_C", "world_actor_sync" },
    { "ufoDropper_tank_C", "world_actor_sync" },
    { "ufoDropper_pig_C", "world_actor_sync" },
};
const char* LaneFor(const std::string& className) {
    for (const auto& e : kLaneOwnedClasses)
        if (className == e.className) return e.lane;
    return nullptr;
}

// Does the registry name `actor` an Npc element: a creature, whose count rides coop/world/creature_event_relay?
bool IsCreature(void* actor) {
    const coop::element::Registry& reg = coop::element::Registry::Get();
    const coop::element::Element* e = reg.Get(reg.EidForActor(actor));
    return e && e->GetType() == coop::element::ElementType::Npc;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
    return s;
}

int ReadRefcount() {
    int32_t n = 0;
    if (AE::EnsureResolved()) AE::ReadCount(n);
    return n;
}

// lib_C::setEvent and its two parameters, from the library's class once it has loaded.
bool ResolveSetEvent() {
    static bool s_failed = false;  // the class loaded without the function or its parameters: said once
    if (g_offContext >= 0) return true;
    if (s_failed) return false;
    void* cls = ue_wrap::object_index::ClassByName(L"lib_C");
    if (!cls) return false;
    g_setEventFn = R::FindFunction(cls, L"setEvent");
    g_offActive = g_setEventFn ? R::FindParamOffset(g_setEventFn, L"isEventActive") : -1;
    const int32_t ctx = g_setEventFn ? R::FindParamOffset(g_setEventFn, L"__WorldContext") : -1;
    if (g_offActive < 0 || ctx < 0) {
        UE_LOGW("event_active: lib_C::setEvent did not resolve (fn=%p isEventActive=%d __WorldContext=%d) -- "
                "no event's begin or end is seen", g_setEventFn, g_offActive, ctx);
        g_setEventFn = nullptr;
        s_failed = true;
        return false;
    }
    g_offContext = ctx;
    return true;
}

// A world or save reload minted a new gamemode with its world, and the old begin times name dead
// senders: drop them (an event still in flight in the new world has no begin the watch saw).
void ForgetOtherWorlds() {
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen == g_beganGen) return;
    g_began.clear();
    g_beganGen = gen;
}

// ---- the reach audit and the target watchdog (host, game thread) -----------------------------
// A BEGIN whose sender is no mapped row, no lane's and not an enrolled creature reaches no client: their activeEvents
// stay 0 while the host's is 1, so the pause menu, the quit and the sleep behave differently per peer. The WARN names
// the class once, so the next such gap is read off the log rather than found by a player.
struct Audit {
    void*       sender;
    int32_t     objIdx;
    std::string className;
    long long   dueMs;
};
std::vector<Audit> g_audit;
std::unordered_set<std::string> g_auditSaid;
constexpr size_t    kMaxAudit = 64;
constexpr long long kAuditDelayMs = 3000;   // the creature's enrolment is drained on the tick after its BeginPlay

void AuditReach(long long now) {
    for (size_t i = 0; i < g_audit.size();) {
        const Audit& a = g_audit[i];
        if (now < a.dueMs) { ++i; continue; }
        const auto b = g_began.find(a.sender);
        // Still counting: began and not ended, in this sender's own slot.
        const bool counting = b != g_began.end() && b->second.objIdx == a.objIdx && R::IsLiveByIndex(a.sender, a.objIdx);
        if (counting && !IsCreature(a.sender) && g_auditSaid.insert(a.className).second)
            UE_LOGW("event_active: BEGIN class=%s reaches no client (no row, lane or npc enrolment) -- clients' "
                    "activeEvents stay 0", a.className.c_str());
        g_audit[i] = std::move(g_audit.back());
        g_audit.pop_back();
    }
}

// An enrolled creature that holds an event count and whose target the host's world no longer has is ended. A
// creature's Blueprint ends its event itself when its target is reached; when a remote peer took or destroyed the
// target first, the host's copy has nothing left to reach and runs on holding the count, so nothing on any peer can
// sleep, save or leave. Narrow by design: each row names the creature, its target class and a grace, and the
// creature is destroyed (its ReceiveDestroyed makes the false, and creature_event_relay sends the 0) only when no live
// target exists for the whole grace. A class whose target cannot be told from the world alone gets no row.
struct TargetWatchdog {
    const wchar_t* creatureClass;
    const wchar_t* targetClass;
    long long      graceMs;
};
constexpr TargetWatchdog kTargetWatchdogs[] = {
    // npc_arirGunStealer_C (event_arirStealsGun_C): the target is prop_arirgun_C. A client that picked the gun up
    // destroyed the host's copy (remote_prop::OnDestroy), and the thief, with no gun, walked at player 0 forever.
    {P::name::NpcClass_ArirGunStealer, P::name::ArirGunClass, 15000},
};
struct GoneSince {
    int32_t   objIdx;
    long long sinceMs;
};
std::unordered_map<void*, GoneSince> g_targetGone;

struct LiveList {
    std::vector<std::pair<void*, int32_t>> objs;
};
void CollectLive(void* ctx, void* obj, int32_t index) {
    if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
    static_cast<LiveList*>(ctx)->objs.emplace_back(obj, index);
}

void WatchTargets(long long now) {
    for (auto it = g_targetGone.begin(); it != g_targetGone.end();)
        it = R::IsLiveByIndex(it->first, it->second.objIdx) ? std::next(it) : g_targetGone.erase(it);
    std::vector<std::pair<void*, int32_t>> toEnd;
    for (const TargetWatchdog& w : kTargetWatchdogs) {
        void* const creatureCls = OI::ClassByName(w.creatureClass);
        if (!creatureCls) continue;
        LiveList creatures;
        OI::ForEachInstance(creatureCls, &CollectLive, &creatures);
        if (creatures.objs.empty()) continue;
        void* const targetCls = OI::ClassByName(w.targetClass);
        if (!targetCls) continue;   // the target's class is not loaded: its absence proves nothing
        LiveList targets;
        OI::ForEachInstance(targetCls, &CollectLive, &targets);
        for (const auto& c : creatures.objs) {
            if (!targets.objs.empty()) { g_targetGone.erase(c.first); continue; }
            auto [g, fresh] = g_targetGone.emplace(c.first, GoneSince{c.second, now});
            if (!fresh && g->second.objIdx != c.second) g->second = GoneSince{c.second, now};
            if (now - g->second.sinceMs >= w.graceMs) toEnd.emplace_back(c.first, c.second);
        }
    }
    for (const auto& c : toEnd) {
        g_targetGone.erase(c.first);
        if (!R::IsLiveByIndex(c.first, c.second)) continue;
        UE_LOGW("event_active: watchdog -- %ls %p has had no target for the grace; destroyed so its event count ends "
                "(count now %d, clients get the 0 from creature_event_relay)",
                R::ClassNameOf(c.first).c_str(), c.first, ReadRefcount());
        E::DestroyActor(c.first);
    }
}

void PumpHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host) return;
    static long long s_lastMs = 0;
    const long long now = NowMs();
    if (now - s_lastMs < 1000) return;   // a once-a-second look
    s_lastMs = now;
    ForgetOtherWorlds();
    AuditReach(now);
    WatchTargets(now);
}

// HOST: an event began or ended, after the game's own bookkeeping ran, so the refcount the lines
// print is the one it left.
void OnSetEventPost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (!ResolveSetEvent() || call.function != g_setEventFn) return;
    void* sender = *reinterpret_cast<void* const*>(call.locals + g_offContext);
    if (!sender || !R::IsLive(sender)) return;
    const bool active = call.locals[g_offActive] != 0;
    ForgetOtherWorlds();
    const long long now = NowMs();
    if (active) {
        const Began began{R::InternalIndexOf(sender), Narrow(R::ClassNameOf(sender)), now};
        auto [it, fresh] = g_began.emplace(sender, began);
        // An entry whose sender died without its setEvent(false) (destroyed mid-event) can share its address
        // with this new sender: the slot check tells them apart, and the new event gets its own begin time.
        if (!fresh && !R::IsLiveByIndex(sender, it->second.objIdx)) {
            it->second = began;
            fresh = true;
        }
        if (fresh) {
            UE_LOGI("event_active: BEGIN class=%s n=%d", it->second.className.c_str(), ReadRefcount());
            // A sender no row, lane or creature enrolment carries reaches no client; the creature's enrolment comes
            // on a later tick, so the judgement is deferred (AuditReach).
            const std::string& cn = it->second.className;
            if (!RowForClass(cn) && !LaneFor(cn) && g_audit.size() < kMaxAudit)
                g_audit.push_back({sender, it->second.objIdx, cn, now + kAuditDelayMs});
        }
        return;
    }
    const auto it = g_began.find(sender);
    if (it == g_began.end()) {
        UE_LOGI("event_active: END class=%s n=%d (began before this host watched)",
                Narrow(R::ClassNameOf(sender)).c_str(), ReadRefcount());
        return;
    }
    UE_LOGI("event_active: END class=%s n=%d elapsed=%llds", it->second.className.c_str(), ReadRefcount(),
            (now - it->second.ms) / 1000);
    g_began.erase(it);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Called every pump tick by the install fanout, which is also the retry until the gate has
    // resolved the watch's name.
    if (!g_watchInstalled)
        g_watchInstalled = sg::WatchName(kRowSetEvent, kTagEventActive, nullptr, &OnSetEventPost);
    if (g_watchInstalled && !g_watchLive) {
        sg::ResolvePendingNames();
        if (sg::NameWatchLive(kRowSetEvent, kTagEventActive)) {
            g_watchLive = true;
            UE_LOGI("event_active: every event's begin and end is seen at lib_C::setEvent (a script-gate watch)");
        }
    }
    PumpHost();
}

void SendJoinSnapshotForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    // The game's own registry is the membership: read at the edge, it holds exactly the events in
    // flight now, the ones that began before this host watched included.
    AE::Senders arr{};
    if (!AE::EnsureResolved() || !AE::ReadSenders(arr) || arr.num < 0 || arr.num > 4096) {
        UE_LOGI("event_active: join-edge slot=%d -- no event registry to read (no world yet)", slot);
        return;
    }
    ForgetOtherWorlds();
    const long long now = NowMs();
    std::vector<void*> sent;
    for (int32_t i = 0; i < arr.num; ++i) {
        void* obj = arr.data ? arr.data[i] : nullptr;
        // The array is a property, so the collector nulls a destroyed sender's entry; one marked
        // for death and not yet collected is skipped here.
        if (!obj || !R::IsLive(obj)) continue;
        bool dup = false;
        for (void* o : sent) dup |= (o == obj);
        if (dup) continue;  // a sender that registered twice is one event
        sent.push_back(obj);
        const std::string className = Narrow(R::ClassNameOf(obj));
        if (const char* lane = LaneFor(className)) {
            UE_LOGI("event_active: join-edge slot=%d class=%s is LANE-OWNED (%s snapshots it) "
                    "-- no EventSnapshot",
                    slot, className.c_str(), lane);
            continue;
        }
        const char* row = RowForClass(className);
        // A creature with no row has no replay to ship: its count rides CreatureEventActive, or stays each
        // viewer's own (coop/world/creature_event_relay). The WARN below stays for classes nothing covers.
        if (!row && IsCreature(obj)) {
            UE_LOGI("event_active: join-edge slot=%d class=%s is a creature -- its count rides CreatureEventActive "
                    "(or stays per viewer), no EventSnapshot", slot, className.c_str());
            continue;
        }
        const auto b = g_began.find(obj);
        const bool known = b != g_began.end() && R::IsLiveByIndex(obj, b->second.objIdx);
        const long long elapsed = known ? (now - b->second.ms) / 1000 : 0;
        coop::net::EventSnapshotPayload p{};  // zero-init -> both name[]s pre-NUL-bound
        std::strncpy(p.className, className.c_str(), sizeof(p.className) - 1);
        if (row) std::strncpy(p.rowName, row, sizeof(p.rowName) - 1);
        p.elapsedSec = static_cast<uint16_t>(elapsed < 0 ? 0 : (elapsed > 65535 ? 65535 : elapsed));
        if (s->SendReliableToSlot(slot, coop::net::ReliableKind::EventSnapshot, &p, sizeof(p))) {
            UE_LOGI("event_active: join-edge slot=%d SNAPSHOT class=%s row=%s elapsed=%s", slot,
                    className.c_str(), row ? row : "<unmapped>",
                    known ? (std::to_string(elapsed) + "s").c_str() : "unknown (began before this host watched)");
        } else {
            UE_LOGW("event_active: join-edge slot=%d EventSnapshot send FAILED (class=%s)",
                    slot, className.c_str());
        }
    }
    if (sent.empty())
        UE_LOGI("event_active: join-edge slot=%d -- 0 in flight (no EventSnapshot needed)", slot);
}

void OnReliable(const coop::net::EventSnapshotPayload& payload) {
    if (!GT::IsGameThread()) { UE_LOGW("event_active: OnReliable off-game-thread -- dropping"); return; }
    // NUL-bound both names (payload crosses the trust boundary; the dispatcher length-checked it).
    char cls[sizeof(payload.className) + 1] = {};
    std::memcpy(cls, payload.className, sizeof(payload.className));
    char row[sizeof(payload.rowName) + 1] = {};
    std::memcpy(row, payload.rowName, sizeof(payload.rowName));
    if (cls[0] == '\0') {
        UE_LOGW("event_active: EventSnapshot with empty className -- dropping");
        return;
    }
    if (row[0] == '\0') {
        // The fill signal: this exact line names the class the map is missing.
        UE_LOGW("event_active: in-flight event class=%s elapsed=%us has NO class->row map entry "
                "-- skipped (add it to kClassRowMap; lanes still deliver lane-owned state)",
                cls, static_cast<unsigned>(payload.elapsedSec));
        return;
    }
    UE_LOGI("event_active: join snapshot -- in-flight class=%s row=%s elapsed=%us",
            cls, row, static_cast<unsigned>(payload.elapsedSec));
    coop::event_fire_sync::ReplayInFlightRow(row);
}

void OnDisconnect() {
    g_audit.clear();
    g_auditSaid.clear();
    g_targetGone.clear();
    g_began.clear();
    g_beganGen = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::event_active_sync

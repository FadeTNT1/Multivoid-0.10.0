// coop/creatures/npc_world_enum.cpp -- see the header. The host-side off-interceptor NPC
// enrolment: the level-load object-array walk and the EX_CallMath deferred-spawn catch, both
// funnelled into one enrol body that reaches the same end state as the interceptor and POST
// pair. The two pieces of host-side npc_sync state this needs (the host-sync-disabled gate
// and the actor-to-eid reverse-map insert) go through npc_sync's public accessors; the NPC
// Element store is the shared mirror manager singleton. The walk's log strings are unchanged
// so existing diagnostics keep matching.

#include "coop/creatures/npc_world_enum.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/element_deleter.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"  // PropMirrors/NpcMirrors/WaMirrors
#include "coop/element/npc.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/creatures/kerfur_entity.h"  // reserve the stable kerfur id when a kerfur NPC is registered
#include "coop/creatures/geom_octa_sync.h"  // OnNpcSpawnSent: the Octa's events follow its spawn's send
#include "coop/creatures/host_born_spawn.h"  // the class catch beside the source rows
#include "coop/creatures/npc_sync.h"
#include "coop/world/world_actor_sync.h"  // HostEnrollExSpawn -- the WA branch of the EX-catch drain
#include "ue_wrap/engine/engine.h"   // TryGetActorLocation / TryGetActorRotation
#include "ue_wrap/actors/kerfur.h"   // HasSaveKey -- the walk's savePersisted gate
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"      // NpcClass_Wisp (the ambient-wisp walk skip)
#include "ue_wrap/core/ufunction_hook.h"   // the EX_CallMath spawn-catch thunk
#include "ue_wrap/facts/function_row.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace coop::npc_world_enum {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

UE_FACT_FN(kRowBeginDeferredSpawn, "npc_world_enum", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass",
           Declared);

// The single host and mirror NPC set, the same singleton the other NPC files wrap.
using coop::element::NpcMirrors;   // canonical accessor (coop/element/mirror_managers.h)

// The one enrol body: allocate, bind, reverse-map, reserve a kerfur id where applicable, and
// broadcast the spawn when connected, for an untracked live NPC actor; the same end state the
// interceptor and POST reach for a fresh spawn. `skipSlot` is the peer slot the broadcast leaves
// out (the joiner whose connect broadcast carries the spawn), -1 for none. Returns the eid, or
// the invalid id on failure (logged). The caller has verified live, allowlisted and untracked.
// Game thread.
coop::element::ElementId EnrollUntrackedNpcActor(void* obj, const std::wstring& clsName,
                                                 bool savePersisted, const char* logTag,
                                                 int skipSlot,
                                                 ue_wrap::FVector* outLoc = nullptr) {
    auto* s = coop::npc_sync::GetSession();
    if (!s) return coop::element::kInvalidId;
    // An NPC whose location or rotation cannot be read is not enrolled, before any element is made
    // for it: the read failing is a dispatch that faulted, so no broadcast or snapshot row could
    // ever place it, and the pose stream would fault on it every tick.
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!ue_wrap::engine::TryGetActorLocation(obj, loc) || !ue_wrap::engine::TryGetActorRotation(obj, rot)) {
        UE_LOGW("npc-sync[%s]: '%ls' actor %p not enrolled -- its location or rotation could not be read",
                logTag, clsName.c_str(), obj);
        return coop::element::kInvalidId;
    }
    if (outLoc) *outLoc = loc;
    auto npc = std::make_unique<coop::element::Npc>();
    std::string typeName8;
    for (size_t k = 0; k < clsName.size() && k < 63; ++k)
        typeName8.push_back(static_cast<char>(clsName[k]));
    npc->SetTypeName(std::move(typeName8));
    // The allocation takes the Registry mutex then the type mutex, the host-authoritative order;
    // the reverse-map insert is a leaf taken separately after, never nested.
    const coop::element::ElementId eid =
        NpcMirrors().AllocAndInstall(std::move(npc), /*isHost=*/true);
    if (eid == coop::element::kInvalidId) {
        UE_LOGW("npc-sync[%s]: AllocAndInstall kInvalidId for '%ls' (Registry full?) -- skipping",
                logTag, clsName.c_str());
        return coop::element::kInvalidId;
    }
    // Bind through a null-checked lookup, the POST observer's pattern: if the freshly allocated
    // Element is not retrievable, drain it back out and leave no reverse-map entry pointing at an
    // unbound Element, which the pose stream and the connect broadcast would iterate forever.
    coop::element::Npc* el = NpcMirrors().Get(eid);
    if (!el) {
        UE_LOGW("npc-sync[%s]: eid=%u not retrievable after AllocAndInstall -- draining (no bind)",
                logTag, eid);
        coop::element::ElementDeleter::Get().Enqueue(NpcMirrors().Take(eid));
        return coop::element::kInvalidId;
    }
    el->SetActor(obj, R::InternalIndexOf(obj));
    coop::npc_sync::MapActorToNpcId(obj, eid);
    // A kerfur NPC also gets a stable host-range kerfur id reserved in the kerfur entity table;
    // host authority, idempotent per actor.
    if (clsName.find(L"kerfurOmega") != std::wstring::npos) {
        coop::kerfur_entity::AllocKerfurId(obj, eid, coop::kerfur_entity::Form::Npc, clsName);
    }
    // Make the registration visible now: broadcast the spawn for the newly registered NPC to the
    // peers already connected. A joiner (skipSlot) gets it in its connect broadcast, which sends
    // every tracked NPC: MTA's join, the joiner announced to the joined players with it skipped,
    // then the others' elements to the joiner alone
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:1426, :1459).
    // Nothing is sent, and nothing logged, when no other peer is world-ready: the sender asks the
    // fan-out's own gate first, where MTA filters inside its fan-out (CPlayerManager.cpp:162-170).
    // MTA registers an element at its creation (reference/mtasa-blue/Server/mods/deathmatch/logic/CElement.cpp:39);
    // a creature that loaded with the level, or that no watched spawn source made, is enrolled here.
    bool spawnSent = false;
    if (s->connected() && s->AnyWorldReadyPeer(skipSlot)) {
        coop::net::EntitySpawnPayload p{};
        const std::string& tn = el->GetTypeName();
        p.className.len = 0;
        for (size_t k = 0; k < tn.size() && k < 63; ++k)
            p.className.data[p.className.len++] = tn[k];
        p.elementId = static_cast<uint32_t>(eid);
        const auto scl = ue_wrap::engine::GetActorScale3D(obj);  // mirror at true size
        p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
        p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
        p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
        p.savePersisted = savePersisted ? 1 : 0;
        // Carry the off-to-active dupe retire key for a window-turned-on kerfur (see
        // npc_pose_host.cpp). Stamped for every enrolment, harmless when no off-prop mirror is bound at
        // that eid, since the eid-keyed sweep finds nothing.
        const coop::element::ElementId offEid = coop::kerfur_entity::GetOriginOffEidForEid(eid);
        p.retireOffEid = (offEid == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(offEid);
        spawnSent = s->SendEntitySpawn(p, skipSlot);
        if (!spawnSent) {
            UE_LOGW("npc-sync[%s]: SendEntitySpawn failed for newly-registered eid=%u",
                    logTag, p.elementId);
        }
    }
    // The Geom Octa's lane sends none of its events before the spawn's send. When no peer was world-ready nothing was
    // sent, and the join's snapshot carries the spawn instead.
    coop::geom_octa_sync::OnNpcSpawnSent(static_cast<uint32_t>(eid), obj, el->GetTypeName().data(),
                                         el->GetTypeName().size(), spawnSent, -1);
    return eid;
}

// The EX_CallMath spawn catch: the spawners whose deferred-spawn output is host-authoritative and
// mirrored, each because its BeginDeferred is bytecode-internal (a CallFunc in its ubergraph, never
// ProcessEvent-dispatched) and no interceptor sees it: the wisps event swarm (up to 32), the piramid
// chain (four killer wisps and a pyramid on the world-actor lane), the ambient sky wisps and night
// eggs (world-anchored, so the host rolls them and a client's ticker is refused in the spawn
// authority), the sell gun's coin mint (the host's own sale and the re-commit of a client's alike;
// without its row the coin allowlist entry is inert), the jellyfish path's seven, the story-event
// spawners' creatures, and the gamemode's night roll (an Octa or an egg). Outputs still pass the allowlist gates in the queue branch below (NPC, else
// world-actor), so no arbitrary EX spawn enrolls. `outputs` narrows a source to the outputs whose
// client copies the spawn authority makes inert: the eventer also mints world actors whose BeginPlay
// plays gameplay on a mirror, so they stay unmirrored until they have refusal rows (the story scenes' rows are
// coop/world/event_scene_refuse's: soltomiaCleaning_C's ATV write and sponge, the obelisk's landing, the hole's
// patrol, the firetanks' AI).
struct ExSpawnSource {
    const wchar_t* cls;
    const wchar_t* const* outputs;   // null-terminated; null: any allowlisted output
};
// The world actors are ventKnocker's, morningGay's, borgRozital's, the fallbody / fallcar droppers' and
// graystank's tank or pig dropper (a dropper's kerfur kill and prop drop are refused on a client in
// spawn_authority, and its generator break by the power grid), and soltoClean's cleaner (its gameplay refused on a
// mirror in event_scene_refuse, its reveal, montage and throw relayed by event_scene_sync).
constexpr const wchar_t* kEventerOutputs[] = {L"ventCrawler_C", L"superEgger_C",
                                              P::name::KockerClass, P::name::MorningUfoClass,
                                              P::name::RozitBorgClass, P::name::UfoDropperBodyClass,
                                              P::name::UfoDropperCarClass, P::name::UfoDropperTankClass,
                                              P::name::UfoDropperPigClass, P::name::SoltomiaCleaningClass,
                                              nullptr};
// The gamemode mints many classes besides these, and most of its other spawners are not refused on a client yet,
// so its source is narrowed to the outputs of the rolls whose client copies the spawn authority refuses: the night
// roll (mainGamemode_C::CustomEvent_5, an Octa or a night egg), the figure roll (CustomEvent_3, a figura_C) and the UFO
// rolls (mainGamemode_C::ufo_ball, ufo_boo, ufo_joel and ufo_pill; ufo_midas mints nothing).
constexpr const wchar_t* kGeomOctaNpcOutputs[] = {L"geomOcta_C", L"eg_C", P::name::NpcClass_Figura, P::name::UfoBallClass,
                                                  P::name::UfoBoofoClass, P::name::UfoJoelClass, P::name::UfoPillfoClass,
                                                  nullptr};
// The gamemode's other natural birth: tickerFunguy (a timer body that rolls a Funguy at a random absolute map
// point at night). A row of its own beside the Octa's, so the two output lists stay separate.
constexpr const wchar_t* kGamemodeFunguyOutputs[] = {P::name::NpcClass_Funguy, nullptr};
// The war event (event_fossilboarwar_C, minted by the host's midnight rollover): its BeginPlay throws up to 32
// fossil hounds under EX_CallMath, and its grayboar_C props ride the prop lane.
constexpr const wchar_t* kFossilWarOutputs[] = {P::name::NpcClass_Fossilhound, nullptr};
constexpr const wchar_t* kUfoKnobOutputs[] = {P::name::UfoKnobClass, nullptr};
constexpr const wchar_t* kUfoUkOutputs[] = {P::name::UfoUkClass, P::name::UfoUkEnemyClass, nullptr};
constexpr const wchar_t* kSuperFogOutputs[] = {P::name::HoelUfoClass, nullptr};
constexpr const wchar_t* kArirFollowerOutputs[] = {P::name::NpcClass_ArirFollower, nullptr};
// boarwar's shooters and its four closing firetanks: its grayboar_C props ride the prop lane; a firetank mirror's
// AI timers are refused in event_scene_refuse.
constexpr const wchar_t* kBoarInvasionOutputs[] = {P::name::NpcClass_AriralShooter, P::name::FiretankClass, nullptr};
// The story scenes' sources (coop/world/event_scene_sync): the obelisk box's prop spawner mints the obelisk (and
// props, which are the prop lane's); the hole controller mints the room (and holeDevice_C, a prop); the patrol
// controller mints the kavotia rings and the igetis.
constexpr const wchar_t* kSpawnPropOutputs[] = {P::name::ObeliskClass, nullptr};
constexpr const wchar_t* kBottomHoleOutputs[] = {P::name::HoleBottomClass, nullptr};
constexpr const wchar_t* kKavotiaPatrolOutputs[] = {P::name::NpcClass_Kavotia, P::name::IgetisClass, nullptr};
constexpr ExSpawnSource kExSpawnSources[] = {
    {L"trigger_wispSwarm_C",     nullptr},          // the wisps event swarm
    {L"piramidSpawner_C",        nullptr},          // the piramid event chain
    {L"ticker_wispSpawner_C",    nullptr},          // the ambient sky wisps
    {L"ticker_egSpawner_C",      nullptr},          // the ambient night eggs, one eg_C per roll
    // The hostile tickers (0.10.0): each rolls inside its ReceiveTick, a client's tick refused in spawn_authority, so
    // the host's birth is the only one and rides the npc lane. insomniac mints insomniac_child_C (a child class of
    // insomniac_C, so the allowlist's super-chain walk takes it) or insomniac_C; yellowWisp a killerwisp_C.
    {L"ticker_fossilhoundSpawner_C", nullptr},      // the fossil hound
    {L"ticker_insomniacSpawner_C",   nullptr},      // the insomniac and its child form
    {L"ticker_yellowWispSpawner_C",  nullptr},      // the killer wisp
    {L"event_fossilboarwar_C",       kFossilWarOutputs},  // the war's hounds (the host's rollover mints the event)
    {L"prop_coingun_C",          nullptr},          // the sell gun's coin mint
    {L"jellyfishPath_C",         nullptr},          // the space jellyfish's seven (world-actor lane)
    {L"trigger_eventer_C",       kEventerOutputs},  // the scheduler's dispatcher
    {L"mainGamemode_C",          kGeomOctaNpcOutputs}, // the night roll's Octa or egg, the spawn menu's Octa, the UFOs
    {L"mainGamemode_C",          kGamemodeFunguyOutputs}, // tickerFunguy's Funguy (a second row: any row of the class may pass)
    {L"grayEventController_C",   nullptr},          // graysforest: spawn() mints the grayTest_C pack
    {L"superEgger_C",            nullptr},          // eggvasion: spwn() mints the eg_C children
    {L"tentacleBallsFollower_C", nullptr},          // tentacleBalls: runTrigger() mints the pack
    // The 0.10.0 UFO encounters: each controller mints its flock in its own ubergraph, and the
    // flock ships are world-actor-lane outputs; the client's controllers are refused in spawn_authority.
    {P::name::UfoKnobControllerClass, kUfoKnobOutputs},  // the knob flock (startEvent)
    {P::name::UfoUkControllerClass, kUfoUkOutputs},    // the UK flock (beginEvent, ufoClass)
    {P::name::UfoUkControllerEnemyClass, kUfoUkOutputs},  // the enemy UK flock (the same beginEvent body)
    // The super fog's UFO: superFog_C::spawnUfo mints it under EX_CallMath; a client's spawnUfo is
    // refused in coop/world/weather_fog and the mirror's attack timers in spawn_authority.
    {P::name::SuperFogClass, kSuperFogOutputs},
    // The arirFollower event: the spawner's runTrigger mints the follower; an npc-lane mirror.
    {P::name::TriggerSpawnFollowingArirClass, kArirFollowerOutputs},
    // boarwar: the controller the eventer mints throws its boars and, a minute in, mints two ariral shooters.
    {P::name::BoarInvasionClass, kBoarInvasionOutputs},
    // obelisk: the obelisk box's trigger_spawnProp_C mints obelisk_C (the Warning Obelisk scene).
    {P::name::TriggerSpawnPropClass, kSpawnPropOutputs},
    // rozitalHole: the controller's walk-in mints the room; the patrol controller its kavotia and igetis.
    {P::name::BottomHoleControllerClass, kBottomHoleOutputs},
    {P::name::KavotiaPatrolControllerClass, kKavotiaPatrolOutputs},
};

// Whether `cls`'s own name is one of `names` (null-terminated). One render, then plain compares.
bool NamesOneOf(void* cls, const wchar_t* const* names) {
    const std::wstring n = R::ToString(R::NameOf(cls));
    for (const wchar_t* const* o = names; *o; ++o)
        if (_wcsicmp(n.c_str(), *o) == 0) return true;
    return false;
}

// A source's output may be a world-actor-lane class (the pyramid): the same catch seam,
// drained to the world-actor sync's own enrol instead of the NPC one. The same name-equality
// walk the world-actor sync uses (allocation-free; each compare renders the name through the
// engine scratch), and it sits strictly behind the source-class match, so ambient spawns
// never reach it.
bool IsWaAllowlistedClass(void* cls) {
    if (!cls) return false;
    const auto& nm = R::NameOf(cls);
    for (size_t i = 0; i < P::name::kWorldActorAllowlistSize; ++i)
        if (R::NameEquals(nm, P::name::kWorldActorAllowlist[i])) return true;
    return false;
}

// Queue entries carry the internal index captured at catch time, so the drain validates with
// the index-paired liveness check; a raw pointer cached across a tick boundary is the recycle
// hazard. Cleared on disconnect.
struct PendingExSpawn { void* actor; int32_t internalIdx; };
std::mutex g_pendingMx;
std::vector<PendingExSpawn> g_pendingExSpawns;        // queued actors awaiting the GT drain
constexpr size_t kMaxPendingExSpawns = 256;           // sanity cap (a swarm is 32)

// The Func-thunk post-native callback: fires for every deferred-spawn dispatch, visible or
// EX_CallMath, deep inside the engine spawn, so it stays cheap. Cheapest gates first: the
// atomic session, role and lifecycle gates run before the source-class name compare, so a
// client exits in nanoseconds for every ambient spawn in the game. It fires before the finish,
// so it only queues; the drain reads the real transform next pump tick. Hosting-gated, not
// connected-gated: an event that fires while the host is alone (the pyramid walking before
// the first client joins) must still enrol its actors, since identity and tracking are
// connection-independent and only the send is peer-dependent, with the join snapshot
// replaying tracked state. A connected gate here silently dropped the pyramid and killer-wisp
// catches, and the joiner saw an empty world mid-event.
void OnBeginDeferredExSpawn(void* /*context*/, void* srcObj, void* spawned) {
    if (!spawned || !srcObj) return;
    // A client's half of the class catch: a Blueprint's birth of a host-born class is dissolved. A no-op off a
    // client session.
    coop::host_born_spawn::OnClientBirth(srcObj, spawned);
    auto* s = coop::npc_sync::GetSession();
    if (!s || s->role() != coop::net::Role::Host) return;
    // IsInstalled is the lifecycle-armed proxy for both lanes (they install together at session
    // start). The NPC-specific disabled gate lives in the drain's NPC branch, so a degraded NPC
    // lifecycle cannot silently drop world-actor catches.
    if (!coop::npc_sync::IsInstalled()) return;
    void* srcCls = R::ClassOf(srcObj);
    if (!srcCls) return;
    void* cls = R::ClassOf(spawned);
    if (!cls) return;
    // One render of the source's name, then plain compares: every host deferred spawn reaches here.
    const std::wstring srcName = R::ToString(R::NameOf(srcCls));
    // A class may own several rows (the gamemode's night roll and its Funguy), each with its own outputs.
    bool sourceKnown = false;
    for (const ExSpawnSource& e : kExSpawnSources)
        if (_wcsicmp(srcName.c_str(), e.cls) == 0) { sourceKnown = true; break; }
    bool outputPermitted = false;
    if (sourceKnown) {
        // [dev] octa_red=no_source, the Octa drill's red arm: the gamemode's row is left out of the catch.
        static const bool s_noGamemodeSource =
            coop::config::ResolveEnum(::coop::config_registry::rows::octa_red) == "no_source";
        if (s_noGamemodeSource && _wcsicmp(srcName.c_str(), L"mainGamemode_C") == 0) return;
        // A scoped source's outputs first: one render per row, before the world-actor list's per-entry renders. The
        // spawn passes when any row of the source class lets this output through.
        for (const ExSpawnSource& e : kExSpawnSources) {
            if (_wcsicmp(srcName.c_str(), e.cls) != 0) continue;
            if (!e.outputs || NamesOneOf(cls, e.outputs)) { outputPermitted = true; break; }
        }
    }
    // A class whose every birth is the host's (coop/creatures/host_born_spawn) passes whatever minted it.
    if (!outputPermitted && !coop::host_born_spawn::HostCatches(srcObj, spawned)) return;
    if (!coop::npc_sync::IsAllowlistedClass(cls) && !IsWaAllowlistedClass(cls)) return;
    const int32_t idx = R::InternalIndexOf(spawned);
    std::lock_guard<std::mutex> lk(g_pendingMx);
    if (g_pendingExSpawns.size() >= kMaxPendingExSpawns) {
        // The cap exists so a stalled drain cannot grow the queue unbounded; a full queue then
        // drops every later catch quietly. Said once: the drop is per-catch, the evidence once.
        static bool s_overflowSaid = false;
        if (!s_overflowSaid) {
            s_overflowSaid = true;
            UE_LOGW("npc-sync[ex-spawn]: pending queue FULL at %zu -- dropping catches until the "
                    "drain runs (a full queue mirrors nothing more this burst)",
                    g_pendingExSpawns.size());
        }
        return;
    }
    g_pendingExSpawns.push_back({spawned, idx});
}

}  // namespace

void InstallExSpawnCatch() {
    // Idempotent per UFunction and callback inside the hook install; it chains after any other
    // Func thunk on the same UFunction (the trash collect's ambient-prop observer), and both fire.
    if (ue_wrap::ufunction_hook::InstallPostHook(ue_wrap::facts::Resolve(kRowBeginDeferredSpawn),
                                                 &OnBeginDeferredExSpawn)) {
        UE_LOGI("npc-sync[ex-spawn]: Func-thunk catch installed on BeginDeferred (source-gated: "
                "trigger_wispSwarm_C -> wisp_C, piramidSpawner_C -> killerwisp_C + piramid2_C, "
                "jellyfishPath_C -> jellyfish_C, trigger_eventer_C -> ventCrawler_C + superEgger_C, "
                "trigger_eventer_C -> kocker_C + morningUfo_C + rozitBorg_C + ufoDropper_body_C + ufoDropper_car_C + "
                "ufoDropper_tank_C + ufoDropper_pig_C, "
                "trigger_eventer_C -> soltomiaCleaning_C, trigger_spawnProp_C -> obelisk_C, "
                "event_bottomHoleController_C -> holeBottom_C, kavotiaPatrolController_C -> kavotia_C + igetis_C, "
                "trigger_spawnFollowingArir_C -> npc_arirFollower_C, boarInvasion_C -> npc_ariral_shooter_C + "
                "firetank_C, "
                "grayEventController_C -> grayTest_C, superEgger_C + ticker_egSpawner_C -> eg_C, "
                "mainGamemode_C -> geomOcta_C + eg_C + npc_funguy_C + ufo_ballfo_C + ufo_boofo_spawn1_C + ufo_joel_C + ufo_pillfo_C, "
                "ticker_fossilhoundSpawner_C + event_fossilboarwar_C -> fossilhound_C, "
                "ticker_insomniacSpawner_C -> insomniac_C + insomniac_child_C, "
                "ticker_yellowWispSpawner_C -> killerwisp_C, "
                "tentacleBallsFollower_C -> tentacleBall_C, the UFO controllers -> ufo_knob_C + ufo_UK_C + "
                "ufo_UK_enemy_C, superFog_C -> HoelUfo_C; and the host-born classes whatever mints them; "
                "EX_CallMath spawns now enroll)");
    } else {
        UE_LOGW("npc-sync[ex-spawn]: InstallPostHook FAILED -- EX_CallMath creature spawns will "
                "NOT mirror this session (event-swarm wisps host-only)");
    }
}

void DrainPendingExSpawns() {
    auto* s = coop::npc_sync::GetSession();
    if (!s || s->role() != coop::net::Role::Host) return;
    std::vector<PendingExSpawn> pending;
    {
        std::lock_guard<std::mutex> lk(g_pendingMx);
        if (g_pendingExSpawns.empty()) return;
        pending.swap(g_pendingExSpawns);
    }
    for (const PendingExSpawn& e : pending) {
        void* obj = e.actor;
        // Index-paired liveness: the catch-time internal index makes a recycled slot read dead
        // instead of validating a different object at the same address.
        if (!obj || !R::IsLiveByIndex(obj, e.internalIdx)) continue;  // died before Finish / recycled
        void* cls = R::ClassOf(obj);
        if (!cls) continue;
        if (!coop::npc_sync::IsAllowlistedClass(cls)) {
            // World-actor-lane output (the pyramid): hand it to the world-actor sync's own enrol,
            // which re-gates on its allowlist and lifecycle, dedups through its reverse map and
            // broadcasts.
            if (IsWaAllowlistedClass(cls))
                coop::world_actor_sync::HostEnrollExSpawn(obj);
            continue;
        }
        // The NPC-specific lifecycle gate: without a working destroy observer an NPC Element would
        // leak, so the enrol is skipped, not the world-actor branch above.
        if (coop::npc_sync::IsHostNpcSyncDisabled()) continue;
        // Dedup against the interceptor and POST path: a dispatched spawn that also matched a
        // source class was already allocated by the PRE and bound by the POST, both before this
        // drain.
        if (coop::npc_sync::GetNpcIdForActor(obj) != coop::element::kInvalidId) continue;
        const std::wstring clsName = R::ToString(R::NameOf(cls));
        // Not save-persisted: an event-swarm spawn happens after any join, so no peer has a local
        // twin.
        ue_wrap::FVector loc{};
        const coop::element::ElementId eid =
            EnrollUntrackedNpcActor(obj, clsName, /*savePersisted=*/false, "ex-spawn", /*skipSlot=*/-1, &loc);
        if (eid != coop::element::kInvalidId) {
            UE_LOGI("npc-sync[ex-spawn]: enrolled '%ls' eid=%u at (%.0f, %.0f, %.0f) "
                    "(EX_CallMath BeginDeferred, source- or class-gated catch)",
                    clsName.c_str(), eid, loc.X, loc.Y, loc.Z);
        }
    }
}

void ClearPendingExSpawns() {
    // The disconnect edge: entries queued in the disconnecting tick must never survive into the
    // next session, since a stale address could recycle into an unrelated live actor and pass
    // the liveness gate. Called from the npc_sync disconnect.
    std::lock_guard<std::mutex> lk(g_pendingMx);
    g_pendingExSpawns.clear();
}

int RegisterExistingWorldNpcs(int joiningSlot) {
    // Host only: register the pre-existing and level-load NPC actors so the connect snapshot
    // mirrors them. See the header. The same end state as the interceptor and POST for a fresh
    // spawn (the Element allocated, the live actor bound, the reverse-map entry), for an actor
    // that loaded with the level.
    auto* s = coop::npc_sync::GetSession();
    if (!s || s->role() != coop::net::Role::Host) return 0;
    // Only when the host NPC lifecycle is fully armed: the interceptor, the POST and the destroy
    // PRE observers installed, and the host broadcast path not disabled (set when the observer
    // table is full). The same gate the interceptor uses: allocating an NPC Element without a
    // guaranteed destroy observer would leak it and drain the host-id free stack. Before the
    // install completes the allowlist is unresolved too, so this would no-op anyway.
    if (!coop::npc_sync::IsInstalled() || coop::npc_sync::IsHostNpcSyncDisabled()) return 0;

    const int32_t n = R::NumObjects();
    int registered = 0, found = 0, alreadyTracked = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        // The fast filter first, matching the prop connect-edge walk: the subclass-aware allowlist
        // check is a few pointer compares over the super chain, and almost all of the object array
        // is not an NPC and never pays for the string allocations below.
        void* cls = R::ClassOf(obj);
        if (!cls || !coop::npc_sync::IsAllowlistedClass(cls)) continue;
        // The liveness guard before the string allocations: skip a purged but not yet reaped slot
        // without paying for a name render; also required before the index read touches the
        // object's memory.
        if (!R::IsLive(obj)) continue;
        // Skip the class-default objects, the class template rather than a world instance.
        const std::wstring objName = R::ToString(R::NameOf(obj));
        if (objName.rfind(L"Default__", 0) == 0) continue;
        ++found;
        // Skip actors already tracked (spawned through the interceptor or the dev path this
        // session, or a prior connect re-scan). The reverse map is the constant-time gate; without
        // it every NPC would be double-registered.
        if (coop::npc_sync::GetNpcIdForActor(obj) != coop::element::kInvalidId) { ++alreadyTracked; continue; }
        const std::wstring clsName = R::ToString(R::NameOf(cls));
        // An untracked plain wisp is left alone here: ticker and swarm output enrols at spawn
        // through the catch, and a joiner reaches tracked wisps through the connect broadcast, not
        // this walk.
        if (clsName == P::name::NpcClass_Wisp) continue;
        const coop::element::ElementId eid = EnrollUntrackedNpcActor(
            obj, clsName,
            // A save-keyed creature is one the joiner may have loaded itself, so its spawn (the
            // connect broadcast's, which builds the same flag) lets the joiner adopt its twin.
            ue_wrap::kerfur::HasSaveKey(obj),
            "world-enum", joiningSlot);
        if (eid == coop::element::kInvalidId) continue;
        ++registered;
    }
    if (found > 0)
        UE_LOGI("npc-sync[world-enum]: registered %d pre-existing world NPC(s) "
                "(%d allowlisted instance(s) in world, %d already tracked; scanned %d GUObjectArray slots)",
                registered, found, alreadyTracked, n);
    return registered;
}

}  // namespace coop::npc_world_enum

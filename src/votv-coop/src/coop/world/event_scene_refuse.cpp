// coop/world/event_scene_refuse.cpp -- the story-event scenes' watches (see coop/world/event_scene_sync.h): on the
// host, the bodies whose entry is a scene's phase edge; on a client, each scene's gameplay bodies, refused. The
// bodies are the 0.10.0 capture's (multivoid-fingerprints-capture.txt); each class's census sits at its rows.
// A row refuses on a mirror only (a client's own copy of the class, a save-loaded firetank, runs as it did) or on
// every client copy (a class whose every client copy must stay inert).

#include "event_scene_detail.h"

#include "coop/net/protocol.h"
#include "coop/player/player_hit.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/world/world_actor_sync.h"

#include "ue_wrap/actors/event_scenes.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <cmath>
#include <cstdint>
#include <iterator>

namespace coop::event_scene_sync::detail {
namespace {

namespace R   = ue_wrap::reflection;
namespace P   = ue_wrap::profile;
namespace E   = ue_wrap::engine;
namespace SG  = ue_wrap::script_gate;
namespace ES  = ue_wrap::event_scenes;
namespace ids = coop::net::event_scene_ids;
namespace ph  = coop::net::event_scene_phases;

// The cleaner: ReceiveBeginPlay cleans the first ATV's dirt and binds the garage door's dooropened to `opened`;
// the box overlap (any actor) shows it, teleports the ATV and jams three doors; `opened` plays the montage; its
// `throw` notify mints the sponge; the completion destroys it, ReceiveDestroyed un-jams the doors.
UE_FACT_FN(kRowSoltoBeginPlay, "event_scene_sync", L"soltomiaCleaning_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowSoltoOverlap, "event_scene_sync", L"soltomiaCleaning_C", L"BndEvt__soltomiaCleaning_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowSoltoOpened, "event_scene_sync", L"soltomiaCleaning_C", L"opened", Declared);
UE_FACT_FN(kRowSoltoNotify, "event_scene_sync", L"soltomiaCleaning_C", L"OnNotifyBegin_CC1C802A418B8136B4D73DA13F1FA965", Declared);
UE_FACT_FN(kRowSoltoCompleted, "event_scene_sync", L"soltomiaCleaning_C", L"OnCompleted_CC1C802A418B8136B4D73DA13F1FA965", Declared);
UE_FACT_FN(kRowSoltoDestroyed, "event_scene_sync", L"soltomiaCleaning_C", L"ReceiveDestroyed", Declared);
// The obelisk: BeginPlay is the rise and fall (kept); A__FinishedFunc is the landing's impulse, seventy rocks,
// prop_obelisk_C, getMainPlayer's punch and `fallen`. The punch's watch delivers it to every client.
UE_FACT_FN(kRowObeliskFinish, "event_scene_sync", L"obelisk_C", L"A__FinishedFunc", Declared);
UE_FACT_FN(kRowPunch, "event_scene_sync", L"mainPlayer_C", L"punch", Declared);
// The hole: the controller's box overlap mints the room, the device and the patrol; the patrol's BeginPlay mints
// the kavotia rings and the igetis. kavotia_C: BeginPlay starts its MoveTo patrol and timers, sense chases,
// openDoors opens or hacks doors, randCharge and layDown move it, alert turns it on a player. igetis_C: BeginPlay
// arms its timers, its tick and sphere overlaps grab props, throw mints them.
UE_FACT_FN(kRowHoleOverlap, "event_scene_sync", L"event_bottomHoleController_C", L"BndEvt__event_bottomHoleController_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowPatrolBeginPlay, "event_scene_sync", L"kavotiaPatrolController_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowKavotiaBeginPlay, "event_scene_sync", L"kavotia_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowKavotiaSense, "event_scene_sync", L"kavotia_C", L"sense", Declared);
UE_FACT_FN(kRowKavotiaOpenDoors, "event_scene_sync", L"kavotia_C", L"openDoors", Declared);
UE_FACT_FN(kRowKavotiaRandCharge, "event_scene_sync", L"kavotia_C", L"randCharge", Declared);
UE_FACT_FN(kRowKavotiaLayDown, "event_scene_sync", L"kavotia_C", L"layDown", Declared);
UE_FACT_FN(kRowKavotiaAlert, "event_scene_sync", L"kavotia_C", L"alert", Declared);
UE_FACT_FN(kRowIgetisBeginPlay, "event_scene_sync", L"igetis_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowIgetisTick, "event_scene_sync", L"igetis_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowIgetisThrow, "event_scene_sync", L"igetis_C", L"throw", Declared);
UE_FACT_FN(kRowIgetisSphere, "event_scene_sync", L"igetis_C", L"BndEvt__igetis_Sphere_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowIgetisSphere1, "event_scene_sync", L"igetis_C", L"BndEvt__igetis_Sphere1_K2Node_ComponentBoundEvent_1_ComponentBeginOverlapSignature__DelegateSignature", Declared);
// The invasion's firetanks: BeginPlay builds the wheels (kept) and arms update, senseUpdate, fire, speak and
// makeHealthNoise; the AI, weapon, senses, contacts and death are refused on a mirror, the two sounds run.
UE_FACT_FN(kRowTankUpdate, "event_scene_sync", L"firetank_C", L"update", Declared);
UE_FACT_FN(kRowTankSenseUpdate, "event_scene_sync", L"firetank_C", L"senseUpdate", Declared);
UE_FACT_FN(kRowTankSensePawn, "event_scene_sync", L"firetank_C", L"sensePawn", Declared);
UE_FACT_FN(kRowTankMoveTo, "event_scene_sync", L"firetank_C", L"moveTo", Declared);
UE_FACT_FN(kRowTankFire, "event_scene_sync", L"firetank_C", L"fire", Declared);
UE_FACT_FN(kRowTankCube, "event_scene_sync", L"firetank_C", L"BndEvt__firetank_Cube_K2Node_ComponentBoundEvent_5_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowTankBackoff, "event_scene_sync", L"firetank_C", L"BndEvt__testTank_backoffCollide_K2Node_ComponentBoundEvent_1_ComponentHitSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowTankCube1, "event_scene_sync", L"firetank_C", L"BndEvt__testTank_Cube1_K2Node_ComponentBoundEvent_3_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowTankSeeIn, "event_scene_sync", L"firetank_C", L"BndEvt__testTank_seeZone_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowTankSeeOut, "event_scene_sync", L"firetank_C", L"BndEvt__testTank_seeZone_K2Node_ComponentBoundEvent_2_ComponentEndOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowTankAddDamage, "event_scene_sync", L"firetank_C", L"addDamage", Declared);
UE_FACT_FN(kRowTankDamageByPlayer, "event_scene_sync", L"firetank_C", L"damageByPlayer", Declared);
UE_FACT_FN(kRowTankCheckIfDead, "event_scene_sync", L"firetank_C", L"checkIfDead", Declared);
UE_FACT_FN(kRowTankExploded, "event_scene_sync", L"firetank_C", L"exploded", Declared);
// The invasion's UFO shader and the prank's trigger: their verbs are the host's phase edges.
UE_FACT_FN(kRowShieldSetActive, "event_scene_sync", L"ufoShieldShader_C", L"setActive", Declared);
UE_FACT_FN(kRowShieldFlyAway, "event_scene_sync", L"ufoShieldShader_C", L"flyaway", Declared);
UE_FACT_FN(kRowAgravCloak, "event_scene_sync", L"trigger_agrav_C", L"cloak", Declared);
// The gamemode's ufo_midas flight: the level-placed UFO's setActive is the host's phase edge.
UE_FACT_FN(kRowMidasSetActive, "event_scene_sync", L"midasufotest_C", L"setActive", Declared);
// The eventer's walk-in saucers: a dropper's box overlap (any actor) and its forceDeploy run one DoOnce body that
// shows it and plays its descent; the morning UFO's box overlap sets `fly` and shows it for a player only. Each is
// the host's phase edge, so a client's mirror starts with the host's and not only on its own player's walk-in.
// The dropper's rows are ufoDropper_C's bodies, which its four children inherit.
UE_FACT_FN(kRowDropperOverlap, "event_scene_sync", L"ufoDropper_C", L"BndEvt__ufoDropper_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowDropperForceDeploy, "event_scene_sync", L"ufoDropper_C", L"forceDeploy", Declared);
UE_FACT_FN(kRowMorningUfoOverlap, "event_scene_sync", L"morningUfo_C", L"BndEvt__morningUfo_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);

// What a row does on each side.
enum class Kind : uint8_t {
    RefuseMirror,   // CLIENT: refused on a world-actor mirror
    RefuseClient,   // CLIENT: refused on every copy
    SoltoOverlap,   // HOST: revealed; CLIENT: refused on a mirror
    SoltoOpened,    // HOST: the montage, when the cleaner is active
    SoltoNotify,    // HOST: thrown on the throw notify; CLIENT: the throw notify refused on a mirror, its look redone
    ObeliskFinish,  // HOST: landed; CLIENT: refused on every copy
    Punch,          // HOST: an obelisk's punch of player 0, delivered to every client
    ShieldSetActive,  // HOST: shown or hidden
    ShieldFlyAway,    // HOST: flying away
    AgravCloak,       // HOST: the shimmer on or off
    MidasSetActive,   // HOST: the flight begun or ended
    DropperDeploy,    // HOST: deployed
    MorningUfoFly,    // HOST (after the body): flying, once the body set `fly`
};

struct Row {
    const ue_wrap::facts::Function* row;
    const char* tag;
    Kind kind;
};
constexpr Row kRows[] = {
    {&kRowSoltoBeginPlay,     "soltomia.ReceiveBeginPlay", Kind::RefuseMirror},
    {&kRowSoltoOverlap,       "soltomia.BoxOverlap",       Kind::SoltoOverlap},
    {&kRowSoltoOpened,        "soltomia.opened",           Kind::SoltoOpened},
    {&kRowSoltoNotify,        "soltomia.OnNotifyBegin",    Kind::SoltoNotify},
    {&kRowSoltoCompleted,     "soltomia.OnCompleted",      Kind::RefuseMirror},
    {&kRowSoltoDestroyed,     "soltomia.ReceiveDestroyed", Kind::RefuseMirror},
    {&kRowObeliskFinish,      "obelisk.A__Finished",       Kind::ObeliskFinish},
    {&kRowPunch,              "mainPlayer.punch",          Kind::Punch},
    {&kRowHoleOverlap,        "bottomHole.BoxOverlap",     Kind::RefuseClient},
    {&kRowPatrolBeginPlay,    "kavotiaPatrol.ReceiveBeginPlay", Kind::RefuseClient},
    {&kRowKavotiaBeginPlay,   "kavotia.ReceiveBeginPlay",  Kind::RefuseClient},
    {&kRowKavotiaSense,       "kavotia.sense",             Kind::RefuseClient},
    {&kRowKavotiaOpenDoors,   "kavotia.openDoors",         Kind::RefuseClient},
    {&kRowKavotiaRandCharge,  "kavotia.randCharge",        Kind::RefuseClient},
    {&kRowKavotiaLayDown,     "kavotia.layDown",           Kind::RefuseClient},
    {&kRowKavotiaAlert,       "kavotia.alert",             Kind::RefuseClient},
    {&kRowIgetisBeginPlay,    "igetis.ReceiveBeginPlay",   Kind::RefuseMirror},
    {&kRowIgetisTick,         "igetis.ReceiveTick",        Kind::RefuseMirror},
    {&kRowIgetisThrow,        "igetis.throw",              Kind::RefuseMirror},
    {&kRowIgetisSphere,       "igetis.SphereOverlap",      Kind::RefuseMirror},
    {&kRowIgetisSphere1,      "igetis.Sphere1Overlap",     Kind::RefuseMirror},
    {&kRowTankUpdate,         "firetank.update",           Kind::RefuseMirror},
    {&kRowTankSenseUpdate,    "firetank.senseUpdate",      Kind::RefuseMirror},
    {&kRowTankSensePawn,      "firetank.sensePawn",        Kind::RefuseMirror},
    {&kRowTankMoveTo,         "firetank.moveTo",           Kind::RefuseMirror},
    {&kRowTankFire,           "firetank.fire",             Kind::RefuseMirror},
    {&kRowTankCube,           "firetank.CubeOverlap",      Kind::RefuseMirror},
    {&kRowTankBackoff,        "firetank.backoffHit",       Kind::RefuseMirror},
    {&kRowTankCube1,          "firetank.Cube1Overlap",     Kind::RefuseMirror},
    {&kRowTankSeeIn,          "firetank.seeZoneIn",        Kind::RefuseMirror},
    {&kRowTankSeeOut,         "firetank.seeZoneOut",       Kind::RefuseMirror},
    {&kRowTankAddDamage,      "firetank.addDamage",        Kind::RefuseMirror},
    {&kRowTankDamageByPlayer, "firetank.damageByPlayer",   Kind::RefuseMirror},
    {&kRowTankCheckIfDead,    "firetank.checkIfDead",      Kind::RefuseMirror},
    {&kRowTankExploded,       "firetank.exploded",         Kind::RefuseMirror},
    {&kRowShieldSetActive,    "ufoShield.setActive",       Kind::ShieldSetActive},
    {&kRowShieldFlyAway,      "ufoShield.flyaway",         Kind::ShieldFlyAway},
    {&kRowAgravCloak,         "agrav.cloak",               Kind::AgravCloak},
    {&kRowMidasSetActive,     "midas.setActive",           Kind::MidasSetActive},
    {&kRowDropperOverlap,     "ufoDropper.BoxOverlap",     Kind::DropperDeploy},
    {&kRowDropperForceDeploy, "ufoDropper.forceDeploy",    Kind::DropperDeploy},
    {&kRowMorningUfoOverlap,  "morningUfo.BoxOverlap",     Kind::MorningUfoFly},
};
constexpr int kRowCount = static_cast<int>(std::size(kRows));
constexpr int kTagBase = 0x45530000;   // 'ES', then the row

// Refusals per row, for the log: the first and then each power of two.
uint64_t g_refused[kRowCount] = {};

SG::Verdict Refuse(int row, void* obj) {
    const uint64_t n = ++g_refused[row];
    if ((n & (n - 1)) == 0)
        UE_LOGI("event_scene[%s]: client-refuse %p (call #%llu)", kRows[row].tag, obj, static_cast<unsigned long long>(n));
    return SG::Verdict::Cancel;
}

// A world-actor mirror, or the one this thread is materialising (its BeginPlay runs inside the finish, before
// the mirror row is installed).
bool IsMirror(void* obj) {
    return coop::world_actor_sync::IsMirroredActor(obj) ||
           (obj && coop::world_actor_sync::MaterializingActor() == obj);
}

bool IsClassNamed(void* obj, const wchar_t* cls) {
    return obj && R::NameEquals(R::NameOf(R::ClassOf(obj)), cls);
}

// HOST: the obelisk's punch of player 0 is every player's. Each client's own body is pushed away from the obelisk
// with the punch's own 500, through the hit lane, which runs it there through the game's own punch.
void DeliverObeliskPunch(const SG::Call& c) {
    void* obelisk = c.callerObject;
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(obelisk, at)) return;
    auto& reg = coop::players::Registry::Get();
    int sent = 0;
    for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        coop::RemotePlayer* rp = reg.Puppet(slot);
        void* body = rp ? rp->GetActor() : nullptr;
        ue_wrap::FVector loc{};
        if (!body || !R::IsLive(body) || !E::TryGetActorLocation(body, loc)) continue;
        float dx = loc.X - at.X, dy = loc.Y - at.Y, dz = loc.Z - at.Z;
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 1.f) { dx = 0.f; dy = 0.f; dz = 1.f; } else { dx /= len; dy /= len; dz /= len; }
        coop::net::PlayerHitPayload p{};
        p.verb = coop::net::kPlayerHitVerbPunch;
        p.bone = coop::player_hit::BoneIndex(L"chest");
        p.damage = 0.f;
        p.v0[0] = p.v1[0] = dx * 500.f;
        p.v0[1] = p.v1[1] = dy * 500.f;
        p.v0[2] = p.v1[2] = dz * 500.f;
        if (coop::player_hit::Deliver(slot, p)) ++sent;
    }
    UE_LOGI("event_scene[obelisk]: the landing's punch delivered to %d client(s)", sent);
}

SG::Verdict OnRowPre(const SG::Call& call) {
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount) return SG::Verdict::Run;
    const bool host = IsHost();
    const bool client = !host && IsActiveClient();
    if (!host && !client) return SG::Verdict::Run;
    void* obj = call.object;
    switch (kRows[row].kind) {
    case Kind::RefuseMirror:
        return client && IsMirror(obj) ? Refuse(row, obj) : SG::Verdict::Run;
    case Kind::RefuseClient:
        return client ? Refuse(row, obj) : SG::Verdict::Run;
    case Kind::SoltoOverlap:
        if (host) NoteHostPhase(ids::kSoltoClean, ph::kSoltoRevealed, obj);
        return client && IsMirror(obj) ? Refuse(row, obj) : SG::Verdict::Run;
    case Kind::SoltoOpened: {
        bool active = false;
        if (host && ES::ReadSoltomiaActive(obj, active) && active)
            NoteHostPhase(ids::kSoltoClean, ph::kSoltoMontage, obj);
        return SG::Verdict::Run;
    }
    case Kind::SoltoNotify: {
        if (!ES::IsSoltomiaThrowNotify(call.function, call.locals)) return SG::Verdict::Run;
        if (host) {
            NoteHostPhase(ids::kSoltoClean, ph::kSoltoThrown, obj);
            return SG::Verdict::Run;
        }
        if (!IsMirror(obj)) return SG::Verdict::Run;
        // The sponge is the host's prop; what the throw shows besides it is redone here.
        ES::SoltomiaThrowPresentation(obj);
        return Refuse(row, obj);
    }
    case Kind::ObeliskFinish:
        if (host) NoteHostPhase(ids::kObelisk, ph::kObeliskLanded, obj);
        return client ? Refuse(row, obj) : SG::Verdict::Run;
    case Kind::Punch:
        if (host && IsClassNamed(call.callerObject, P::name::ObeliskClass)) DeliverObeliskPunch(call);
        return SG::Verdict::Run;
    case Kind::ShieldSetActive: {
        bool enabled = false;
        if (host && ES::ReadBoolParam(call.function, call.locals, P::name::UfoShieldEnabledParam, enabled))
            NoteHostPhase(ids::kBoarShield, enabled ? ph::kShieldShown : ph::kShieldHidden, obj);
        return SG::Verdict::Run;
    }
    case Kind::ShieldFlyAway:
        if (host) NoteHostPhase(ids::kBoarShield, ph::kShieldFlyAway, obj);
        return SG::Verdict::Run;
    case Kind::MidasSetActive: {
        bool active = false;
        if (host && ES::ReadBoolParam(call.function, call.locals, P::name::MidasUfoActiveParam, active))
            NoteHostPhase(ids::kMidas, active ? ph::kMidasRun : ph::kMidasEnd, obj);
        return SG::Verdict::Run;
    }
    case Kind::DropperDeploy:
        if (host) NoteHostPhase(ids::kDropper, ph::kDropperDeployed, obj);
        return SG::Verdict::Run;
    case Kind::MorningUfoFly:
        return SG::Verdict::Run;   // the post reads what the body decided
    case Kind::AgravCloak: {
        bool forward = false;
        if (host && ES::ReadBoolParam(call.function, call.locals, P::name::AgravForwardParam, forward)) {
            NoteHostPhase(ids::kAgrav, forward ? ph::kAgravCloakOn : ph::kAgravCloakOff, obj);
            AgravHostEdge(obj, forward);
        }
        return SG::Verdict::Run;
    }
    }
    return SG::Verdict::Run;
}

// After the body: the morning UFO's overlap sets `fly` only for a player, which its own flag tells.
void OnRowPost(const SG::Call& call) {
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount || kRows[row].kind != Kind::MorningUfoFly || !IsHost()) return;
    bool fly = false;
    if (ES::ReadMorningUfoFly(call.object, fly) && fly) NoteHostPhase(ids::kMorningUfo, ph::kMorningUfoFly, call.object);
}

}  // namespace

void InstallWatches() {
    int watched = 0;
    for (int i = 0; i < kRowCount; ++i) {
        const SG::PostFn post = kRows[i].kind == Kind::MorningUfoFly ? &OnRowPost : nullptr;
        if (SG::WatchName(*kRows[i].row, kTagBase + i, &OnRowPre, post)) {
            ++watched;
        } else {
            UE_LOGE("event_scene: the watch on %ls::%ls did not register -- its scene edge or refusal is lost",
                    kRows[i].row->Class(), kRows[i].row->Name());
        }
    }
    UE_LOGI("event_scene: %d/%d scene watches registered by class and name, each live once its class loads",
            watched, kRowCount);
}

}  // namespace coop::event_scene_sync::detail

// coop/world/spawn_authority.cpp -- see the header. The rows rest on these bytecode facts: the
// mushroom master arms one looping spawn timer at begin-play, and its spawn mints spawner children
// with a lifespan, so refused children are reaped by the engine independently of the refused event;
// the mushroom spawner's own looping timer materialises the food cap and self-destroys, and with
// spawn refused the lifespan reaps it; the yellow-wisp ticker spawns at a navmesh random-walk point,
// not around the player, and its product is host-mirrored, so the client must not run its own
// spawner; the sky-wisp ticker spawns the sky wisps at absolute map coordinates, so the host rolls
// and clients mirror through the source-gated catch and the variant allowlist; the roach master's
// summon and its three looping timer entries fire independently of its tick. The tick rows: the
// insomniac and fossilhound tickers roll inside their tick with no delay chains or reap duties, so
// refusing the tick stops the roll and the product, and the products are host-mirrored; the roach
// master's tick drives roach movement, the food-eat mutation and crush traces, which a client running
// it would diverge, so it and its summoner are refused while the roach sync drives the client
// population. The jellyfish path's spawn makes seven fish inside its graph, and the host's are
// mirrored through the source-gated catch, so a client's own, its 18:00 roll's, is refused.

#include "coop/world/spawn_authority.h"

#include "coop/net/session.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <iterator>

namespace coop::spawn_authority {
namespace {

namespace SG = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};

// The facts rows: the body a client must not run, named by its class and its function. The function
// is spelled as the live header dump spells it; names compare without case.
UE_FACT_FN(kRowSpawnMushroomMaster, "spawn_authority", L"mushroomMaster_C", L"Spawn", Declared);
UE_FACT_FN(kRowSpawnMushroomSpawner, "spawn_authority", L"mushroomSpawner_C", L"Spawn", Declared);
UE_FACT_FN(kRowReceiveTickTickerYellowWispSpawner, "spawn_authority", L"ticker_yellowWispSpawner_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowReceiveTickTickerWispSpawner, "spawn_authority", L"ticker_wispSpawner_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowReceiveTickTickerEgSpawner, "spawn_authority", L"ticker_egSpawner_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowSpawnJellyfishPath, "spawn_authority", L"jellyfishPath_C", L"spawn", Declared);
UE_FACT_FN(kRowSummonRoachCockroachMaster, "spawn_authority", L"cockroachMaster_C", L"summonRoach", Declared);
UE_FACT_FN(kRowAddRoachTimerCockroachMaster, "spawn_authority", L"cockroachMaster_C", L"addRoachTimer", Declared);
UE_FACT_FN(kRowSpawnNestTimerCockroachMaster, "spawn_authority", L"cockroachMaster_C", L"spawnNestTimer", Declared);
UE_FACT_FN(kRowCustomEventCockroachMaster, "spawn_authority", L"cockroachMaster_C", L"CustomEvent", Declared);
UE_FACT_FN(kRowReceiveTickTickerInsomniacSpawner, "spawn_authority", L"ticker_insomniacSpawner_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowReceiveTickTickerFossilhoundSpawner, "spawn_authority", L"ticker_fossilhoundSpawner_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowReceiveTickCockroachMaster, "spawn_authority", L"cockroachMaster_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowReceiveTickTickerRoachSummoner, "spawn_authority", L"ticker_roachSummoner_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowReceiveBeginPlayVentCrawler, "spawn_authority", L"ventCrawler_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowBoxBeginOverlapVentCrawler, "spawn_authority", L"ventCrawler_C", L"BndEvt__ventCrawler_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowReceiveBeginPlayGrayTest, "spawn_authority", L"grayTest_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowSphereBeginOverlapGrayTest, "spawn_authority", L"grayTest_C", L"BndEvt__grayTest_Sphere_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowDeacCamsGrayTest, "spawn_authority", L"grayTest_C", L"deacCams", Declared);
UE_FACT_FN(kRowDisableCamsGrayTest, "spawn_authority", L"grayTest_C", L"disableCams", Declared);
UE_FACT_FN(kRowReceiveBeginPlayEg, "spawn_authority", L"eg_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowReceiveTickEg, "spawn_authority", L"eg_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowOnLandedEg, "spawn_authority", L"eg_C", L"OnLanded", Declared);
UE_FACT_FN(kRowRetrieveEg, "spawn_authority", L"eg_C", L"retrieve", Declared);
UE_FACT_FN(kRowReceiveBeginPlayTentacleBall, "spawn_authority", L"tentacleBall_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowReceiveTickTentacleBall, "spawn_authority", L"tentacleBall_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowMoveTentacleBall, "spawn_authority", L"tentacleBall_C", L"move", Declared);
UE_FACT_FN(kRowSphereHitTentacleBall, "spawn_authority", L"tentacleBall_C", L"BndEvt__tentacleBall_Sphere_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowBeginGrayEventController, "spawn_authority", L"grayEventController_C", L"begin", Declared);
UE_FACT_FN(kRowTurnedonGrayEventController, "spawn_authority", L"grayEventController_C", L"turnedon", Declared);
UE_FACT_FN(kRowSpawnGrayEventController, "spawn_authority", L"grayEventController_C", L"spawn", Declared);
UE_FACT_FN(kRowTriggerboxOverlapGrayEventController, "spawn_authority", L"grayEventController_C", L"BndEvt__grayEventController_triggerbox_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowRunTriggerTentacleBallsFollower, "spawn_authority", L"tentacleBallsFollower_C", L"runTrigger", Declared);
UE_FACT_FN(kRowReceiveTickTentacleBallsFollower, "spawn_authority", L"tentacleBallsFollower_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowReceiveBeginPlaySuperEgger, "spawn_authority", L"superEgger_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowSpwnSuperEgger, "spawn_authority", L"superEgger_C", L"spwn", Declared);
UE_FACT_FN(kRowCustomEvent5Gamemode, "spawn_authority", L"mainGamemode_C", L"CustomEvent_5", Declared);
// The 0.10.0 UFO encounters (untitled_1): the bodies the controllers' and the ships' ubergraphs own.
UE_FACT_FN(kRowStartEventKnobController, "spawn_authority", L"ufo_knob_controller_C", L"startEvent", Declared);
UE_FACT_FN(kRowStopEventKnobController, "spawn_authority", L"ufo_knob_controller_C", L"stopEvent", Declared);
UE_FACT_FN(kRowReceiveTickKnobController, "spawn_authority", L"ufo_knob_controller_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowBeginEventUkController, "spawn_authority", L"ufo_UK_controller_C", L"beginEvent", Declared);
UE_FACT_FN(kRowStopEventUkController, "spawn_authority", L"ufo_UK_controller_C", L"stopEvent", Declared);
UE_FACT_FN(kRowReceiveTickUkController, "spawn_authority", L"ufo_UK_controller_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowSetEnemiesUkController, "spawn_authority", L"ufo_UK_controller_C", L"setEnemies", Declared);
UE_FACT_FN(kRowBeginEventCalvine, "spawn_authority", L"ufo_calvine_C", L"beginEvent", Declared);
UE_FACT_FN(kRowAttemptFallCalvine, "spawn_authority", L"ufo_calvine_C", L"attemptFall", Declared);
UE_FACT_FN(kRowStartFallingCalvine, "spawn_authority", L"ufo_calvine_C", L"startFalling", Declared);
UE_FACT_FN(kRowGetupCalvine, "spawn_authority", L"ufo_calvine_C", L"getup", Declared);
UE_FACT_FN(kRowChangeSpeedKnob, "spawn_authority", L"ufo_knob_C", L"changeSpeed", Declared);
UE_FACT_FN(kRowTryToShootUk, "spawn_authority", L"ufo_UK_C", L"tryToShoot", Declared);
UE_FACT_FN(kRowShootLazerUk, "spawn_authority", L"ufo_UK_C", L"shootLazer", Declared);
UE_FACT_FN(kRowChangeSpeedUk, "spawn_authority", L"ufo_UK_C", L"changeSpeed", Declared);
UE_FACT_FN(kRowSetEnemyTargetUk, "spawn_authority", L"ufo_UK_C", L"setEnemyTarget", Declared);
UE_FACT_FN(kRowExplodeUk, "spawn_authority", L"ufo_UK_C", L"explode", Declared);
// The super fog's UFO (HoelUfo_C): the timer bodies its BeginPlay arms.
UE_FACT_FN(kRowAttackHoelUfo, "spawn_authority", L"HoelUfo_C", L"attack", Declared);
UE_FACT_FN(kRowAttackBeamHoelUfo, "spawn_authority", L"HoelUfo_C", L"attackBeam", Declared);
UE_FACT_FN(kRowChangeSpeedHoelUfo, "spawn_authority", L"HoelUfo_C", L"changeSpeed", Declared);
UE_FACT_FN(kRowChangeSpringDirHoelUfo, "spawn_authority", L"HoelUfo_C", L"changeSpringDir", Declared);
// The fallbody / fallcar droppers (ufoDropper_C, the parent of ufoDropper_body_C and ufoDropper_car_C):
// the 10 s kerfur-kill timer its BeginPlay arms, and the timeline's drop event that mints the prop.
UE_FACT_FN(kRowKillKerfurUfoDropper, "spawn_authority", L"ufoDropper_C", L"killKerfur", Declared);
UE_FACT_FN(kRowDropUfoDropper, "spawn_authority", L"ufoDropper_C", L"Timeline_0__Drop__EventFunc", Declared);

// One row: the facts row a client must not run, and the tag its refusal log carries.
struct Row {
    const ue_wrap::facts::Function* row;
    const char* tag;
};
constexpr Row kRows[] = {
    {&kRowSpawnMushroomMaster,            "mushroomMaster.Spawn"},
    {&kRowSpawnMushroomSpawner,           "mushroomSpawner.Spawn"},
    // A late-game class: keyed by name, the watch holds from the class's first body, whenever it loads.
    {&kRowReceiveTickTickerYellowWispSpawner, "yellowWispSpawner.ReceiveTick"},
    // Sky wisps: world-anchored, so the host rolls; the source-gated catch and the variant
    // allowlist mirror the products.
    {&kRowReceiveTickTickerWispSpawner,   "wispSpawner.ReceiveTick"},
    // The night-egg ticker: the same world-anchored shape -- its tick re-arms a 60-300 s
    // interval and rolls a weighted night spawn of one eg_C at random absolute map coordinates,
    // writing gamemode->eg (ticker_egSpawner_C UG:6-44). The host rolls, the catch mirrors the
    // egg; a client's own tick would mint a second, unsynced egg.
    {&kRowReceiveTickTickerEgSpawner,     "egSpawner.ReceiveTick"},
    // The space jellyfish: the host's run is mirrored through the source-gated catch.
    {&kRowSpawnJellyfishPath,             "jellyfishPath.spawn"},
    // The roach sim's entries: the ticker's cross-object call and the three looping timer
    // delegates. The roach sync drives the client population instead.
    {&kRowSummonRoachCockroachMaster,     "cockroachMaster.summonRoach"},
    {&kRowAddRoachTimerCockroachMaster,   "cockroachMaster.addRoachTimer"},
    {&kRowSpawnNestTimerCockroachMaster,  "cockroachMaster.spawnNestTimer"},
    {&kRowCustomEventCockroachMaster,     "cockroachMaster.CustomEvent"},
    // The ticks.
    {&kRowReceiveTickTickerInsomniacSpawner,   "insomniacSpawner.ReceiveTick"},
    {&kRowReceiveTickTickerFossilhoundSpawner, "fossilhoundSpawner.ReceiveTick"},
    {&kRowReceiveTickCockroachMaster,     "cockroachMaster.ReceiveTick"},
    {&kRowReceiveTickTickerRoachSummoner, "roachSummoner.ReceiveTick"},

    // The story-event creatures: on a client every instance is a host mirror -- the local spawn
    // paths are suppressed through the allowlist or refused below -- so their gameplay entries
    // are refused class-wide; the eventer's own verbs are event_fire_sync's. The AnimBP and the
    // host's pose stream stay; what a refused BeginPlay also set up is lost on the mirror: the
    // crawler's footsteps, vent break and metal-break emitter, a ball's and a gray's mesh
    // attach to its spring arm, a ball's ambient loop.
    // ventCrawler_C: the whole scene is the BeginPlay body -- it binds the prop_vent_C ref,
    // plays the moveTL timeline (which itself SetActorLocations the crawl), arms the vent-bang
    // and footstep events, and ends in the door writes, the setEvent flag and K2_DestroyActor
    // (ventCrawler_C UG @2669). The box overlap is the end-of-crawl contact: a client pawn
    // touching the mirror must not open the doors locally (@2363).
    {&kRowReceiveBeginPlayVentCrawler,    "ventCrawler.ReceiveBeginPlay"},
    {&kRowBoxBeginOverlapVentCrawler,     "ventCrawler.BoxBeginOverlap"},
    // grayTest_C: BeginPlay re-attaches the mesh, waits a second and starts the wander MoveTo
    // (@1117); the 3 s loops arm from gatherTeam and step. The sphere overlap is the player
    // catch: on a mainPlayer touch it calls grayController->despawn() (@1508). disableCams is the
    // timer entry that calls deacCams (@1620), which deactivates the client's cameras, rdrone and kerfur.
    {&kRowReceiveBeginPlayGrayTest,       "grayTest.ReceiveBeginPlay"},
    {&kRowSphereBeginOverlapGrayTest,     "grayTest.SphereBeginOverlap"},
    {&kRowDeacCamsGrayTest,               "grayTest.deacCams"},
    {&kRowDisableCamsGrayTest,            "grayTest.disableCams"},
    // eg_C: BeginPlay arms the wander (@1466); the tick bobs the mesh and calls retrieve() once
    // player 0's camera is within 5000 (@15) -- refusing it costs the bob, while letting it run
    // lets a client egg leave on its own. retrieve() is the fly-away despawn whose Delay loop
    // writes rendered and setEvent (@1024, @1476). OnLanded arms check and a 10-300 s
    // retrieve (@1869).
    {&kRowReceiveBeginPlayEg,             "eg.ReceiveBeginPlay"},
    {&kRowReceiveTickEg,                  "eg.ReceiveTick"},
    {&kRowOnLandedEg,                     "eg.OnLanded"},
    {&kRowRetrieveEg,                     "eg.retrieve"},
    // tentacleBall_C: BeginPlay resolves the anim instance, starts Timeline_0 and the ambient
    // loop, and arms the 15-30 s timer and the first move (@7603). The tick is the brain:
    // proximity scans, the pindown/chase decision, the stare updates (@8872). move() is the
    // MoveTo verb it reaches (@10046); the sphere hit is the melee contact (@10314). The
    // montage-notify talk/light callbacks and step() stay -- sound and light presentation.
    {&kRowReceiveBeginPlayTentacleBall,   "tentacleBall.ReceiveBeginPlay"},
    {&kRowReceiveTickTentacleBall,        "tentacleBall.ReceiveTick"},
    {&kRowMoveTentacleBall,               "tentacleBall.move"},
    {&kRowSphereHitTentacleBall,          "tentacleBall.SphereHit"},

    // The event spawners themselves: a client's copies must never mint children. Every
    // BeginDeferred inside these graphs is bytecode-internal, so the spawn interceptor never
    // sees them; refusing the bodies that contain them is the only suppression that holds.
    // grayEventController_C (level-placed on both sides): spawn() is the only body with the
    // grayTest BeginDeferred (@2617, the marker loop) -- it also plays the Audio, writes
    // isRaining=false and runs the deac-cams SphereOverlap. begin() (@3064) is the eventer's
    // entry -- the player punch, the transformer break, the car zap -- and turnedon (@3388)
    // and the triggerbox overlap (@3309) are its local activations.
    {&kRowBeginGrayEventController,       "grayEventController.begin"},
    {&kRowTurnedonGrayEventController,    "grayEventController.turnedon"},
    {&kRowSpawnGrayEventController,       "grayEventController.spawn"},
    {&kRowTriggerboxOverlapGrayEventController, "grayEventController.triggerboxOverlap"},
    // tentacleBallsFollower_C (level-placed on both sides): runTrigger is the five-ball spawn
    // loop plus the setBuddies pass and the cleanup timers (@3432). The tick moves the follow
    // marker along the spline and writes setEvent at the resume and end points (@2012); the
    // client's copy has an empty balls array and nothing to guide.
    {&kRowRunTriggerTentacleBallsFollower,  "tentacleBallsFollower.runTrigger"},
    {&kRowReceiveTickTentacleBallsFollower, "tentacleBallsFollower.ReceiveTick"},
    // superEgger_C: a client's copy is a world-actor mirror, but FinishSpawning runs its
    // BeginPlay, which builds the spawn grid and chains spwn() -- the only body with the eg_C
    // BeginDeferred (@1399, @1316). Refusing both keeps the mirror a husk.
    {&kRowReceiveBeginPlaySuperEgger,     "superEgger.ReceiveBeginPlay"},
    {&kRowSpwnSuperEgger,                 "superEgger.spwn"},
    // mainGamemode_C: the night roll. CustomEvent_5 is a one-shot timer's body, and the body is the roll AND its
    // own re-arm (eleven blocks of the graph: the 3 % night roll that mints a Geom Octa or a night egg, and the next
    // 120-300 s timer), so a client's timer simply ends here. The host rolls and the source-gated catch mirrors the
    // Octa and the egg; the level-placed egg roller is already a row above.
    {&kRowCustomEvent5Gamemode,           "gamemode.CustomEvent_5"},

    // The 0.10.0 UFO encounters. The knob controller starts its flock 0.2 s after its BeginPlay and
    // re-rolls it from its tick; the UK controller starts 5 s after BeginPlay when beginSpawn is set,
    // re-arms itself after a war's stopEvent and picks enemies on a 2 s timer; the enemy controller is
    // a subclass and runs these same bodies. Every flock ship is minted inside these bodies
    // (BeginDeferred under EX_CallMath), so they are refused on a client, and the host's ships reach it
    // through the source-gated catch onto the world-actor lane (npc_world_enum). stopEvent destroys the
    // client's own (empty) flock array only, so refusing it costs nothing and keeps a stray war re-arm
    // (its Delay -> beginEvent) from ever starting.
    {&kRowStartEventKnobController,       "ufoKnobController.startEvent"},
    {&kRowStopEventKnobController,        "ufoKnobController.stopEvent"},
    {&kRowReceiveTickKnobController,      "ufoKnobController.ReceiveTick"},
    {&kRowBeginEventUkController,         "ufoUkController.beginEvent"},
    {&kRowStopEventUkController,          "ufoUkController.stopEvent"},
    {&kRowReceiveTickUkController,        "ufoUkController.ReceiveTick"},
    {&kRowSetEnemiesUkController,         "ufoUkController.setEnemies"},
    // ufo_calvine_C (level-placed on both sides): beginEvent arms the 0.1 s fall-attempt timer, whose
    // attemptFall rolls startFalling (the physics drop, an explosion_c, a barrel roll); getup is the
    // cheat menu's lift. The host's own event runs; a client's copy keeps its spline flight only.
    {&kRowBeginEventCalvine,              "ufoCalvine.beginEvent"},
    {&kRowAttemptFallCalvine,             "ufoCalvine.attemptFall"},
    {&kRowStartFallingCalvine,            "ufoCalvine.startFalling"},
    {&kRowGetupCalvine,                   "ufoCalvine.getup"},
    // The ships' own timers on a client: every client instance is a world-actor mirror (its actor tick
    // is off and its body does not simulate), but BeginPlay still arms the knob's and the UK ship's
    // changeSpeed timers and the UK ship's tryToShoot and enemy timers. shootLazer mints the laser
    // (laserClass) and explode starts the crash; both are the host's, so a mirror runs neither.
    {&kRowChangeSpeedKnob,                "ufoKnob.changeSpeed"},
    {&kRowTryToShootUk,                   "ufoUk.tryToShoot"},
    {&kRowShootLazerUk,                   "ufoUk.shootLazer"},
    {&kRowChangeSpeedUk,                  "ufoUk.changeSpeed"},
    {&kRowSetEnemyTargetUk,               "ufoUk.setEnemyTarget"},
    {&kRowExplodeUk,                      "ufoUk.explode"},
    // The super fog's UFO on a client is a world-actor mirror too (tick off, pose streamed), but its
    // BeginPlay arms a 1 s attack timer (attack -> attackBeam: a sphere overlap that mints
    // hoelUfoAttack_C beams, each igniting and damaging its target) and the 5 s speed and 10 s heading
    // re-rolls. The beams are the host's; the re-rolls only steer a tick the mirror does not run.
    {&kRowAttackHoelUfo,                  "hoelUfo.attack"},
    {&kRowAttackBeamHoelUfo,              "hoelUfo.attackBeam"},
    {&kRowChangeSpeedHoelUfo,             "hoelUfo.changeSpeed"},
    {&kRowChangeSpringDirHoelUfo,         "hoelUfo.changeSpringDir"},
    // The eventer's droppers on a client are world-actor mirrors (the eventer's source row). A player's
    // overlap still shows the mirror and plays its timeline, the saucer's descent on its own component,
    // but the drop event's prop is the host's (it crosses as a prop), and the kerfur kill (a random
    // kerfur dropped to its prop every 10 s) is the host's. The tick's light blink is presentation.
    {&kRowKillKerfurUfoDropper,           "ufoDropper.killKerfur"},
    {&kRowDropUfoDropper,                 "ufoDropper.drop"},
};
constexpr int kRowCount = static_cast<int>(std::size(kRows));
constexpr int kTagBase = 0x53410000;   // 'SA', then the row

// Refusals per row, for the log: the first and then each power of two, so a refused tick proves itself
// without a line a second. Game thread, as the gate's callbacks are.
std::uint64_t g_refused[kRowCount] = {};

SG::Verdict OnRowPre(const SG::Call& call) {
    if (!IsActiveClientSession()) return SG::Verdict::Run;
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount) return SG::Verdict::Run;
    const std::uint64_t n = ++g_refused[row];
    if ((n & (n - 1)) == 0)
        UE_LOGI("spawn_authority[%s]: client-refuse %p (call #%llu)", kRows[row].tag, call.object,
                static_cast<unsigned long long>(n));
    return SG::Verdict::Cancel;
}

std::atomic<bool> g_watched{false};

}  // namespace

// Refuse only while an active client session exists: the running flag flips true in the session
// start and false in the stop, which every disconnect path reaches; a bare role gate would bleed the
// refusal into single player after the session.
bool IsActiveClientSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Client;
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched.exchange(true, std::memory_order_acq_rel)) return;
    int watched = 0;
    for (int i = 0; i < kRowCount; ++i) {
        if (SG::WatchName(*kRows[i].row, kTagBase + i, &OnRowPre, nullptr)) {
            ++watched;
        } else {
            UE_LOGE("spawn_authority: the watch on %ls::%ls did not register -- a client runs it",
                    kRows[i].row->Class(), kRows[i].row->Name());
        }
    }
    UE_LOGI("spawn_authority: %d/%d spawner watches registered by class and name, each live once the game "
            "thread resolves its names; refused only on an active client session", watched, kRowCount);
}

}  // namespace coop::spawn_authority

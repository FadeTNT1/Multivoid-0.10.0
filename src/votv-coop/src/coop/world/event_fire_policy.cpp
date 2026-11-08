// coop/world/event_fire_policy.cpp -- see coop/world/event_fire_sync.h. The replay policy alone
// lives here so the dupe matrix can grow a row at a time free of the sync logic's size budget.

#include "event_fire_policy.h"

#include <cctype>
#include <string>

#include "ue_wrap/core/sdk_profile_names.h"

namespace coop::event_fire_sync {

// The replay policy, the dupe matrix: every row's concrete output and which lane already
// carries it. The default is no replay. Replay only rows whose effect is a deterministic
// level, save or cosmetic flip that no existing lane delivers; replaying a lane-covered row
// would double-deliver. Keyed by name only: the few names living in both dispatchers have
// the same verdict either way, and the replay call still uses the received dispatch kind.
const char* const kReplayRows[] = {
    // Story and save flips (level-placed triggers; no lane):
    "treehouse_0", "treehouse_1", "treehouse_2", "treehouse_3", "treehouse_4", "treehouse_5",
    "break_RomeoSierra", "break_Victor", "break_Victor2",
    // Force-object appends (a save array the client's own dish scan reads; no lane):
    "looker_0-1", "looker_1-1", "looker_2-1", "looker_3-1", "looker_4-1",
    "arirSignal", "arirSpk", "picSignal", "peace",
    "arirSat_0", "arirSat_1", "arirSat_2", "piramid_sig",
    // Cosmetic or sound with no lane (the solar row's lights-dark converges with the light lane,
    // the same resulting state, echo-suppressed by its last-known prime):
    "solar", "call0",
    // The alien sounds: the special mints three invisible noiser_C walkers at fixed points, each
    // wandering around ITS machine's player 0 and playing outsideAlien_cue every 5-10 s until seen or
    // 60-120 s pass. Per-viewer scare sounds with no shared output, so the host's fire is the cue and
    // each peer hears its own (the replay is the client's one eventer call):
    "alienSounds",
    // Trigger-box scare arms, per-viewer scares by design; arming both sides is the correct coop
    // semantics (each player gets the scare on their own overlap):
    "toeStab", "falseEnter", "mann", "vent", "crys", "fakeGrays", "susArir",
    // The ship (0.10.0): the level-placed arirShipAppear_C fades in at runTrigger(0) and fades out a second
    // after ITS machine's camera has seen it, so each peer gets its own sighting. It spawns nothing; the
    // sixteen emails its fade-out mails are the host's (a client's own finish is refused in spawn_authority).
    "arirShip",
    // Wake-up relocations, per player by design: bedEvent's trigger_bedEvent_C mints a bedEvent_C, and the
    // treehouseSleep special an event_arirTreehouseSleep_C, both save actors that wait for their machine's
    // gamemode `wokenUp` and then teleport getMainPlayer (bedEvent first sets the bed to its own transform,
    // the same on every peer) and destroy themselves. Each peer's copy moves its own player on its own wake;
    // the host's save carries the host's copy to a joiner, whose load then does the same:
    "bedEvent", "treehouseSleep",
    // The oil special mints seven oilStainSpawn_C at fixed points; each traces down once its spot is unseen and
    // leaves one grime_oil_C decal there, the same spot on every peer, so a peer's own copy is the host's stain
    // (grime keys by position, so a wipe crosses on the grime lane):
    "oil",
    // Graffiti decal specials (a grime decal spawn; no lane):
    "arirGraff_0", "arirGraff_1", "arirGraff_2", "arirGraff_3",
    "arirGraff_4", "arirGraff_5", "arirGraff_6",
};

struct NoReplayRow { const char* name; const char* lane; };
const NoReplayRow kNoReplayRows[] = {
    // Outputs already ride a lane (a replay is a double delivery):
    { "starRain", "event_cue lane (cue 0)" },
    { "arirFollower", "npc lane (trigger_spawnFollowingArir_C EX-catch)" },
    // The swarm's wisps ride the npc lane (the source-gated catch in npc_world_enum); a replay
    // would arm the client's own swarm trigger and double-spawn client-local creatures on top of
    // the mirrors:
    { "wisps", "npc lane (EX-catch; event-swarm wisp_C mirrored)" },
    // The pyramid's path is host-random (wander and chase timers), so a replay armed the client's
    // own trigger box and a client walk-in spawned a divergent client-local pyramid with
    // unmirrored wisps. The arrival comes by mirror: the world-actor pose stream, npc-lane wisps
    // and the pyramid sync's brain suppression and gather relay.
    { "piramid", "piramid mirror lane (WA pose + piramid_sync brain/gather)" },
    // The obelisk's graph is not a bare flag flip: obelisk_C spawns prop_C/prop_obelisk_C actors
    // and punches getMainPlayer -- a replay would double-spawn the props client-local and hit the
    // client's own player. The host's obelisk is mirrored, its landing relayed and its punch delivered to every
    // client's own body (coop/world/event_scene_sync); its props ride the prop lane:
    { "obelisk", "event scene lane (obelisk_C mirror + landing phase; props on the prop lane; punch on player_hit)" },
    // earthTp fires newsky_C.tp: the black hole, sky and ambience flip plus two 2D sounds and
    // emails. The emails ride the host's append feed, and a replay would add local copies; the sky
    // and the sounds ride SkyState (its black-hole state and tp count), a client's own tp refused:
    { "earthTp", "sky_sync (SkyState black hole + tp cue; emails ride the host append feed)" },
    { "vehtp", "atv lane" },
    { "picnic", "prop lane" }, { "destroyPicnic", "prop lane" },
    { "enasus", "prop lane" }, { "enacros", "prop lane" },
    { "cookier", "prop lane (armed prop)" }, { "paperGray", "prop lane (armed prop)" },
    { "arirEgg", "prop lane (armed prop)" },
    { "console", "device lanes" }, { "lightswitch", "device lanes" },
    { "keypadGuess", "device lanes" },
    // atvExplode only sets car.trap on the host's gamemode ATV (runSpecialEvent @312); the ATV lane carries that
    // flag to every copy in the stamp's bit 4, so the event itself stays host-only (a client's car need not be the
    // host's), and the trap's blast is the host's (the ATV health drill's `trap` arm):
    { "atvExplode", "ATV lane (car.trap in the stamp's bit 4)" },
    // The prank's physics is the host's (its props ride the prop lane with their gravity, the props near each
    // client floated with the host's own); its shimmer and hover sound are relayed (coop/world/event_scene_sync).
    // A replay would float a second, divergent set on the client:
    { "agrav", "event scene lane (cloak phase) + prop lane (kNoGravity; props near clients floated by the host)" },
    // Creature and save-actor spawns, host-only until mirrored. The crawler, the egger's eggs, the
    // grays and the tentacle balls are mirrored; the eventer's other outputs are not (its catch
    // enrols only what a client's copy is kept inert for, npc_world_enum), so they stay host-only:
    { "ventCrawler", "npc lane (eventer EX-catch); a client's walk-in run on the host; its footsteps and vent "
                     "break on the sound relay" },
    { "ventKnocker", "world-actor lane (eventer EX-catch: kocker_C)" },
    { "tentacleBalls", "npc lane (follower EX-catch)" },
    { "morningGay", "world-actor lane (eventer EX-catch: morningUfo_C)" },
    { "borgRozital", "world-actor lane (eventer EX-catch: rozitBorg_C, its tick kept on the mirror)" },
    { "graysforest", "npc lane (controller EX-catch)" },
    // graystank mints a ufoDropper_tank_C (the eventer's `combat` set) or a ufoDropper_pig_C: the dropper is a
    // world-actor mirror whose kerfur kill, drop and generator break a client refuses.
    { "graystank", "world-actor lane (eventer EX-catch: ufoDropper_tank_C / ufoDropper_pig_C)" },
    // arirBuster mints arirBusterSpawner_C, which destroys itself in its BeginPlay; its crates, blinker lamp and
    // notebook are props the prop lane carries:
    { "arirBuster", "prop lane (arirBusterSpawner_C's crates, lamp and notebook)" },
    { "eggvasion", "npc and world-actor lanes (eventer and egger EX-catch)" },
    // boarwar mints boarInvasion_C: its grayboar_C props ride the prop lane, its two ariral shooters the npc lane
    // and its four end-of-war firetank_C the world-actor lane (its source row; a mirror's AI refused), and the level
    // UFO shader it lights and flies away rides the event scene lane:
    { "boarwar", "prop lane (grayboar_C), npc lane (npc_ariral_shooter_C), world-actor lane (firetank_C), "
                 "event scene lane (ufoShieldShader_C)" },
    // soltoClean's soltomiaCleaning_C is hidden until a player's overlap, which then teleports the ATV, closes
    // and jams three doors and later throws a sponge prop. The host's runs it all; a client's mirror has that
    // gameplay refused and shows the reveal, the montage and the throw the host relays:
    { "soltoClean", "event scene lane (soltomiaCleaning_C mirror + reveal/montage/throw phases)" },
    { "salt", "save-actor spawn (no lane)" },
    // rozitalHole's event_bottomHoleController_C (a save actor) waits for an overlap at the far hole, then mints
    // holeBottom_C, holeDevice_C and a kavotiaPatrolController_C (igetis_C, kavotia_C) beside prop swaps the
    // prop lane carries. The room and the igetis are world-actor mirrors, the kavotia npc mirrors, their brains
    // refused on a client, as is a client's own controller's overlap (coop/world/event_scene_sync):
    { "rozitalHole", "world-actor lane (holeBottom_C, igetis_C) + npc lane (kavotia_C), props on the prop lane" },
    // dreamer_dreambase_C is not loaded in the capture the 0.10.0 pass read; unread:
    { "dreambase", "save-actor spawn (no lane; dreamer_dreambase_C unread)" },
    { "fallbody_0", "world-actor lane (eventer EX-catch: ufoDropper_body_C; drop and kerfur kill refused)" },
    { "fallbody_1", "world-actor lane (eventer EX-catch: ufoDropper_body_C; drop and kerfur kill refused)" },
    { "fallcar_0", "world-actor lane (eventer EX-catch: ufoDropper_car_C; drop and kerfur kill refused)" },
    // The prank layer (host-local RNG; thrown-prop outputs ride the prop lane). These spawn their props at once:
    { "food", "prank special (prop lane)" }, { "drive", "prank special (prop lane)" },
    { "poisonFood", "prank special (prop lane)" }, { "expDrive", "prank special (prop lane)" },
    { "cookiebox", "prank special (prop lane)" }, { "bombBox", "prank special (prop lane)" },
    { "begos", "prank special (prop lane: begoExplosion_C's spikeballs)" },
    // These arm a box whose overlap spawns the props (any pawn's, a puppet's included, on the host):
    { "trashPiles", "prank special (prop lane, overlap-armed: event_trashPiles_C)" },
    { "vaccine", "prank special (prop lane, overlap-armed: event_vaccine_C)" },
    { "gascans", "prank special (prop lane, overlap-armed: event_funnyGascans_C)" },
    { "hillRoller", "prank spawner (prop lane, overlap-armed: hillRollerSpawner_C)" },
    // alienJump's alien shows on its overlap and throws props and nails at the host's camera; its body and its
    // sound are the host's:
    { "alienJump", "prank spawner (prop lane, overlap-armed: alienJump_C aims at the host's camera)" },
    // rockThrow's three rockThrower_C throw prop_rock_C only while getPlayerPawn(0), the host's own player,
    // stands in their box; a client in it is not aimed at:
    { "rockThrow", "prank spawner (prop lane: rockThrower_C aims at the host's player only)" },
    // event_arirFuelsATV_C refuels or repairs an ATV parked in its box once unseen; fuel and health ride the ATV
    // condition lane, the ariral visitor it mints is the host's:
    { "atvFuel", "ATV condition lane (event_arirFuelsATV_C fuel)" },
    { "atvFix", "ATV condition lane (event_arirFuelsATV_C health)" },
    { "trashBase", "prank spawner (no lane)" },
};

// The interaction rows: the row's entire effect is the prank special, host-local RNG.
static bool IsPrankRow(const std::string& n) {
    return n.rfind("arirInteraction_", 0) == 0;
}

// Row names compare without case: the game spells a row by its own casing ('Salt', 'Console' on 0.10.0, which
// the 0.9.0 policy rows spelled lowercase), so an exact compare sent them to the unknown-row default.
static bool SameName(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return i == a.size() && !b[i];
}

int ReplayVerdict(const std::string& name, const char** laneOut) {
    for (const char* r : kReplayRows)
        if (SameName(name, r)) return 1;
    for (const auto& nr : kNoReplayRows)
        if (SameName(name, nr.name)) { *laneOut = nr.lane; return 0; }
    if (IsPrankRow(name)) { *laneOut = "prank special (host-local RNG)"; return 0; }
    return -1;
}

const wchar_t* RequiredKey(const std::string& name) {
    if (name.rfind("treehouse_", 0) == 0) return ue_wrap::profile::name::TreehouseSaveKey;
    return nullptr;
}

}  // namespace coop::event_fire_sync

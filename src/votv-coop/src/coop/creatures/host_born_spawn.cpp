// coop/creatures/host_born_spawn.cpp -- see the header. Each row's producers are the 0.10.0 capture's
// BeginDeferredActorSpawnFromClass sites of its class, every one an EX_CallMath call.

#include "coop/creatures/host_born_spawn.h"

#include "coop/props/prop_echo_suppress.h"   // InMirrorSpawnScope: a spawn of ours, never dissolved
#include "coop/world/spawn_authority.h"      // IsActiveClientSession

#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine_pawn.h"      // DestroyActor

#include <cstdint>

namespace coop::host_born_spawn {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace UH = ue_wrap::ufunction_hook;
namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::engine;

// A row: the class (it and every class below it), and the only producers it covers, or null for any producer but
// the gamemode. A row with producers is client-only: its class reaches a client through a lane of its own.
struct Row {
    const wchar_t*        cls;
    const wchar_t* const* sources;   // null-terminated
};
constexpr const wchar_t* kStolasSources[] = {P::name::StolasProtectionVolumeClass, nullptr};
constexpr Row kRows[] = {
    // The zombie family: the horde's stack, a corpse's and the skerfuro's summons, the alien dropper's aliens. A
    // client's mirrors of the family have their graphs refused (coop/creatures/npc_mirror_brain), so mint nothing.
    {P::name::NpcClass_Zombie,          nullptr},
    {P::name::NpcClass_Krampus,         nullptr},   // the Christmas tree's new-day roll (spawnKrampus)
    {P::name::NpcClass_GoreSlither,     nullptr},   // the slither spawner, and the burrow prop's BeginPlay
    {P::name::NpcClass_AriralShooter,   nullptr},   // boarwar's two and the treehouse's
    {P::name::NpcClass_AriralPigBeater, nullptr},   // the treehouse's
    {P::name::NpcClass_KillerWisp,      nullptr},   // the pyramid's, the yellow ticker's and the alien dropper's
    {P::name::NpcClass_EriePlushLive,   nullptr},   // the plush's fuck, a chopped Erie's regrowth, reducerep's two
    {P::name::NpcClass_AngryErieFlesh,  nullptr},   // the plush's anger and the cremator's timerbang
    {P::name::NpcClass_Rufus,           nullptr},   // the goose's break and the console
    {P::name::UfoAlienDropperClass,     nullptr},   // the alien summoner's landing and its death
    {P::name::ArirShipClass,            nullptr},   // the alien dropper's BeginPlay
    {P::name::LightningStrikeClass,     kStolasSources},  // the volume's strike at the pawns inside it
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));
constexpr int kMaxSources = 2;

R::FName g_rowName[kRowCount] = {};
R::FName g_sourceName[kRowCount][kMaxSources] = {};
int      g_sourceCount[kRowCount] = {};
R::FName g_gamemode = {};
bool     g_resolved = false;
uint64_t g_dissolved[kRowCount] = {};

bool Same(const R::FName& a, const R::FName& b) {
    return a.ComparisonIndex == b.ComparisonIndex && a.Number == b.Number;
}

// The row whose class is `cls` or a class above it, or -1. Two integers per row per hop.
int RowOf(void* cls) {
    int hops = 0;
    for (void* c = cls; c && hops < 16; c = R::SuperStructOf(c), ++hops) {
        const R::FName& n = R::NameOf(c);
        for (int i = 0; i < kRowCount; ++i)
            if (Same(n, g_rowName[i])) return i;
    }
    return -1;
}

// Is `src` the gamemode, or an object of a class below it?
bool IsGamemode(void* src) {
    int hops = 0;
    for (void* c = R::ClassOf(src); c && hops < 16; c = R::SuperStructOf(c), ++hops)
        if (Same(R::NameOf(c), g_gamemode)) return true;
    return false;
}

// Is `src`'s own class one of the row's producers?
bool SourceListed(int row, void* src) {
    void* cls = R::ClassOf(src);
    if (!cls) return false;
    const R::FName& n = R::NameOf(cls);
    for (int i = 0; i < g_sourceCount[row]; ++i)
        if (Same(n, g_sourceName[row][i])) return true;
    return false;
}

bool Convert(const wchar_t* text, R::FName& out) {
    out = ue_wrap::fname_utils::StringToFName(text);
    return out.ComparisonIndex != 0 || out.Number != 0;
}

}  // namespace

void ResolveNames() {
    if (g_resolved || !ue_wrap::fname_utils::Resolved()) return;
    bool ok = Convert(P::name::GamemodeClass, g_gamemode);
    for (int i = 0; ok && i < kRowCount; ++i) {
        ok = Convert(kRows[i].cls, g_rowName[i]);
        g_sourceCount[i] = 0;
        for (const wchar_t* const* s = kRows[i].sources; ok && s && *s && g_sourceCount[i] < kMaxSources; ++s)
            ok = Convert(*s, g_sourceName[i][g_sourceCount[i]++]);
    }
    if (!ok) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGW("host_born_spawn: a row's name did not convert -- nothing is caught or dissolved until it does");
        }
        return;
    }
    g_resolved = true;
    UE_LOGI("host_born_spawn: %d class rows live (host: caught on every Blueprint route; client: dissolved)",
            kRowCount);
}

bool HostCatches(void* sourceObject, void* spawned) {
    if (!g_resolved || !sourceObject || !spawned) return false;
    const int row = RowOf(R::ClassOf(spawned));
    if (row < 0 || g_sourceCount[row] > 0) return false;
    return !IsGamemode(sourceObject);
}

void OnClientBirth(void* sourceObject, void* spawned) {
    if (!g_resolved || !sourceObject || !spawned) return;
    if (!coop::spawn_authority::IsActiveClientSession()) return;
    // A Blueprint's call only: ProcessEvent's route (our mirror spawns, the dev spawns) stands at no bytecode.
    if (UH::CurrentCallerFrame().codeOffset < 0) return;
    if (coop::prop_echo_suppress::InMirrorSpawnScope()) return;
    const int row = RowOf(R::ClassOf(spawned));
    if (row < 0) return;
    if (g_sourceCount[row] > 0 ? !SourceListed(row, sourceObject) : IsGamemode(sourceObject)) return;
    // The Blueprint reads a null result (FinishSpawningActor passes a null actor through); the unfinished actor is
    // destroyed by a posted task, never inside the native call still on the stack, and validated by its slot.
    const int32_t index = R::InternalIndexOf(spawned);
    if (void** r = static_cast<void**>(UH::CurrentResult())) *r = nullptr;
    GT::Post([spawned, index] {
        if (R::IsLiveByIndex(spawned, index)) E::DestroyActor(spawned);
    });
    const uint64_t n = ++g_dissolved[row];
    if ((n & (n - 1)) == 0)
        UE_LOGI("host_born_spawn: client dissolved '%ls' minted by %ls (#%llu); the host's arrives as a mirror",
                kRows[row].cls, R::ClassNameOf(sourceObject).c_str(), static_cast<unsigned long long>(n));
}

}  // namespace coop::host_born_spawn

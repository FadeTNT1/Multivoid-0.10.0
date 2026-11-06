// ue_wrap/engine/save_restore_watch.cpp -- see ue_wrap/engine/save_restore_watch.h.

#include "ue_wrap/engine/save_restore_watch.h"

#include "ue_wrap/actors/inventory_component.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <string>
#include <vector>

namespace N = ue_wrap::profile::name;

UE_FACT_FN(kRowRestoreObjects, "save_load", L"mainGamemode_C", L"loadObjects", Declared);
UE_FACT_FN(kRowRestorePlayer, "save_load", L"mainPlayer_C", L"betterLoadData", Declared);

namespace ue_wrap::save_restore_watch {

namespace R = reflection;
namespace SG = script_gate;
namespace WI = world_identity;

namespace {

constexpr int kTagObjects = 0x53524F42;  // 'SROB'
constexpr int kTagPlayer = 0x5352504C;   // 'SRPL'

bool g_objectsWatch = false;
bool g_playerWatch = false;
bool g_held = false;

// A world identity as a record keeps it: the pointer and its object-array slot, compared with the
// current world without touching the (possibly freed) world.
struct WorldStamp {
    void* world = nullptr;
    int32_t idx = -1;
    void Set(void* w) {
        world = w;
        idx = w ? R::InternalIndexOf(w) : -1;
    }
    bool IsCurrent() const {
        if (!world) return false;
        void* const now = WI::CurrentWorld();
        return now == world && R::IsLiveByIndex(world, idx);
    }
};

WorldStamp g_objectsWorld;
Restore g_objectsResult = Restore::NotSeen;
std::wstring g_levelAtEntry;
bool g_levelNamed = false;
WorldStamp g_playerWorld;

// The players loadObjects has handed a record to in the pass that is running. A save made while a second
// mainPlayer_C stood in the world (a peer's puppet, the skins preview's mannequin) carries one record per body,
// all under the key "mainPlayer"; the loader hands every record of a key to the one live player, so the later,
// empty record empties the inventory the first one just restored. The first record a player gets in a pass is the
// player's own (the world's own player is enumerated before any body made later), so a second is refused.
bool g_inObjects = false;
std::vector<void*> g_loadedPlayers;
int g_duplicatesRefused = 0;

// The game instance's currentLevel, as text ("None" for NAME_None), or empty when unresolved.
std::wstring ReadCurrentLevel(bool& named) {
    named = false;
    void* gi = world_singleton::GameInstance();
    if (!gi) return {};
    static int32_t s_off = -2;
    if (s_off == -2) s_off = R::FindPropertyOffset(R::ClassOf(gi), N::GameInstanceCurrentLevelProp);
    if (s_off < 0) return {};
    const R::FName n = *reinterpret_cast<const R::FName*>(reinterpret_cast<const uint8_t*>(gi) + s_off);
    named = !(n.ComparisonIndex == 0 && n.Number == 0);
    return named ? R::ToString(n) : std::wstring(L"None");
}

SG::Verdict OnObjectsPre(const SG::Call&) {
    g_levelAtEntry = ReadCurrentLevel(g_levelNamed);
    g_inObjects = true;
    g_loadedPlayers.clear();
    g_duplicatesRefused = 0;
    return SG::Verdict::Run;
}

void OnObjectsPost(const SG::Call& c) {
    g_inObjects = false;
    g_loadedPlayers.clear();
    if (g_duplicatesRefused > 0)
        UE_LOGW("save_restore: loadObjects refused %d extra player record(s) for a player that had already loaded "
                "its own -- the save was written while another mainPlayer_C stood in the world",
                g_duplicatesRefused);
    void* world = WI::WorldOf(c.object);
    g_objectsWorld.Set(world);
    if (g_levelNamed) {
        g_objectsResult = Restore::Ran;
        UE_LOGI("save_restore: mainGamemode_C::loadObjects ran in world %p with currentLevel '%ls' -- the "
                "save's player transform, objects and triggers were restored", world, g_levelAtEntry.c_str());
    } else {
        g_objectsResult = Restore::Skipped;
        UE_LOGE("save_restore: mainGamemode_C::loadObjects ran in world %p with currentLevel '%ls' -- the game "
                "skipped the whole restore (\"Cannot load objects: current level none\"); this world holds "
                "none of the save's objects, inventory or position", world,
                g_levelAtEntry.empty() ? L"<unresolved>" : g_levelAtEntry.c_str());
    }
}

SG::Verdict OnPlayerPre(const SG::Call& c) {
    if (!g_inObjects) return SG::Verdict::Run;
    for (void* seen : g_loadedPlayers) {
        if (seen != c.object) continue;
        ++g_duplicatesRefused;
        UE_LOGW("save_restore: mainPlayer_C::betterLoadData on %p refused -- a second player record for a player that "
                "already loaded one in this pass (it would overwrite the inventory and equipment just restored)",
                c.object);
        return SG::Verdict::Cancel;
    }
    g_loadedPlayers.push_back(c.object);
    return SG::Verdict::Run;
}

void OnPlayerPost(const SG::Call& c) {
    {
        const inventory_component::Counts n = inventory_component::CountItems(c.object);
        UE_LOGI("save_restore: betterLoadData on pawn %p (component %p) holds carried=%d hands=%d equipment=%d "
                "slotC=%d slotD=%d", c.object, inventory_component::Of(c.object), n.carried, n.hands, n.equipment,
                n.slotC, n.slotD);
    }
    void* world = WI::WorldOf(c.object);
    const bool fresh = !g_playerWorld.IsCurrent() || g_playerWorld.world != world;
    g_playerWorld.Set(world);
    if (fresh)
        UE_LOGI("save_restore: mainPlayer_C::betterLoadData returned on %p in world %p -- the player's saved "
                "data (inventory, equipment) applied", c.object, world);
}

}  // namespace

void Arm(const char* who) {
    if (!g_objectsWatch) g_objectsWatch = SG::WatchName(kRowRestoreObjects, kTagObjects, &OnObjectsPre, &OnObjectsPost);
    if (!g_playerWatch) g_playerWatch = SG::WatchName(kRowRestorePlayer, kTagPlayer, &OnPlayerPre, &OnPlayerPost);
    if (g_objectsWatch || g_playerWatch) SG::ResolvePendingNames();
    if (!g_held && g_objectsWatch) {
        SG::Acquire("save_restore_watch");
        g_held = true;
        UE_LOGI("save_restore: watching the save's restore for %s (loadObjects watch %s, betterLoadData watch %s)",
                who, SG::NameWatchLive(kRowRestoreObjects, kTagObjects) ? "live" : "pending",
                SG::NameWatchLive(kRowRestorePlayer, kTagPlayer) ? "live" : "pending");
    }
}

void Disarm() {
    if (!g_held) return;
    g_held = false;
    SG::Release("save_restore_watch");
}

View ForCurrentWorld() {
    View v;
    v.watchLive = g_objectsWatch && SG::IsInstalled() && SG::NameWatchLive(kRowRestoreObjects, kTagObjects);
    if (g_objectsWorld.IsCurrent()) {
        v.objects = g_objectsResult;
        v.currentLevel = g_levelAtEntry;
    }
    v.playerLoaded = g_playerWorld.IsCurrent();
    return v;
}

}  // namespace ue_wrap::save_restore_watch

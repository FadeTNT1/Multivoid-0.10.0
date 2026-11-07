// ue_wrap/engine/engine_save.cpp -- the save load, the game-mode derivation, the campaign-scoped
// save cache, the fresh New Game boot and the return to the main menu. Declared in
// ue_wrap/engine/engine_save.h, which the engine.h umbrella includes; everything here is
// game-thread only. The load path is a boot poll: LoadStorySave and
// StartFreshGame are retried until the save class and the world are live.

#include "ue_wrap/engine/engine.h"
#include "engine_save_internal.h"  // co-located private header (src tree, not include/)

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/save_restore_watch.h"
#include "ue_wrap/engine/world_folder.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/engine/world_meta.h"
#include "ue_wrap/world/game_mode.h"
#include "ue_wrap/world/game_rules.h"
#include "ue_wrap/world/world_singleton.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace ue_wrap::engine {

namespace P = profile;
namespace R = reflection;

namespace {
// Cached across the boot poll. The GameplayStatics CDO and its UFunction are native and rooted;
// setSaveSlotObject belongs to mainGameInstance_C, whose class lives as long as the persistent
// GameInstance.
void* g_storyGsCdo = nullptr;
void* g_loadGameFn = nullptr;
void* g_setSaveSlotFn = nullptr;

// The cached save object is campaign-scoped, not process-scoped: a campaign is one continuous poll
// sequence targeting one slot. Within it the gameInstance.saveSlotObject property we register into
// keeps the object alive; once the campaign ends the game may replace that reference and the object
// can be purged at any world transition. So a new campaign forces a disk reload, and IsLiveByIndex
// guards the reuse within one. Caching across campaigns instead planted a dangling pointer into the
// GameInstance and the world was rebuilt from freed memory.
void* g_storySave = nullptr;          // cached USaveGame* (one disk load per campaign)
int32_t g_storySaveIdx = -1;          // its GUObjectArray index (IsLiveByIndex guard)
std::wstring g_storySaveSlot;         // campaign identity axis 1: the target slot or world key
// A world folder's other two objects, registered with the save (LoadWorld); null for a flat slot.
void* g_storyMeta = nullptr;
int32_t g_storyMetaIdx = -1;
void* g_storyPersist = nullptr;
int32_t g_storyPersistIdx = -1;
// The second axis: the non-gameplay world the campaign polls in. Every return to the menu
// creates a new menu world, so a changed polling world means a world round-trip happened since
// the cache was built, whether or not any poll saw the gameplay in between.
void* g_campaignWorld = nullptr;
int32_t g_campaignWorldIdx = -1;

// StartFreshGame's pseudo-slot: the blank save is registered under this name, which doubles as
// that path's campaign identity.
constexpr const wchar_t* kFreshSlotName = L"coop_client_fresh";

// The coop inventory layer registers this to overwrite a freshly loaded or created save's
// player inventory before the native loadObjects materialises it.
SaveObjectReadyHook g_saveObjectReadyHook = nullptr;
// Fired once per loaded or created save object; the hook self-gates to a no-op off a join.
void FireSaveObjectReadyHook(void* saveObj) {
    if (!g_saveObjectReadyHook || !saveObj) return;
    UE_LOGI("engine: firing SaveObjectReadyHook on save object %p (pre-materialize)", saveObj);
    g_saveObjectReadyHook(saveObj);
}

// The game stores a save's mode only in the slot-name prefix: getSavePrefix(mode) yields the
// prefix for a mode, and a slot whose name starts with it is that mode. The menu writes
// mainGameInstance.GameMode from this on load; the load bypass here has to do the same, or a
// story save loads in the default sandbox mode.
void* g_saveSlotsUiCdo  = nullptr;  // the ui_saveSlots_C CDO; getSavePrefix is a pure mode-to-prefix map
int32_t g_saveSlotsUiCdoIdx = -1;   // its GUObjectArray index; a BP CDO can be collected with its class
void* g_getSavePrefixFn = nullptr;  // ui_saveSlots_C::getSavePrefix(mode) -> FString prefix
bool  g_gameModeApplied = false;    // the GameMode is derived and written once per campaign
// SetFreshGameRules' pending list; read only on the game thread, in StartFreshGame.
std::vector<std::pair<std::string, std::string>> g_freshGameRules;

// This campaign issued the travel into a level, so "in gameplay" waits for the save's restore in
// that level (save_restore_watch). A poll that finds the game already in gameplay without having
// opened it has no restore of ours to wait for.
bool g_launchIssued = false;
// The restore watch was live when the last open was issued, so a restore in that level is seen.
bool g_restoreObservable = false;
// When this campaign first found its pawn in the level, for the restore wait's cap.
std::chrono::steady_clock::time_point g_restoreWaitStart{};
// How long a pawn may stand in the level with no loadObjects seen before the boot gives up waiting.
constexpr std::chrono::seconds kRestoreWaitCap{30};

// The FName property `prop` of `obj`, or NAME_None when it does not resolve (then `ok` is false).
R::FName ReadFNameProp(void* obj, const wchar_t* prop, bool& ok) {
    ok = false;
    if (!obj || !R::IsLive(obj)) return R::FName{0, 0};
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), prop);
    if (off < 0) return R::FName{0, 0};
    ok = true;
    return *reinterpret_cast<const R::FName*>(reinterpret_cast<const uint8_t*>(obj) + off);
}
bool WriteFNameProp(void* obj, const wchar_t* prop, const R::FName& value) {
    if (!obj || !R::IsLive(obj)) return false;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), prop);
    if (off < 0) return false;
    *reinterpret_cast<R::FName*>(reinterpret_cast<uint8_t*>(obj) + off) = value;
    return true;
}
std::wstring FNameText(const R::FName& n) {
    return (n.ComparisonIndex == 0 && n.Number == 0) ? std::wstring(L"None") : R::ToString(n);
}
bool SameLevelName(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    return true;
}

// The level state the game's own launch leaves before its travel, which the console open this
// boot travels with does not: the save menu's trylaunch sets baseLevel = the metadata's starterMap
// and calls lib_C::loadLevel(starterMap, ...), which sets currentLevel = the level on the game
// instance and on the metadata (and, on its way back to the menu, currentLevel = None). The new
// level's gamemode restores the save in loadObjects only while currentLevel names a level; left at
// None the restore is skipped whole and the world opens with none of the save's objects, the
// player at the spawn with an empty inventory -- and the next save writes that over the world.
// lib_C::loadLevel itself is not called: it saves the metadata to disk and travels through the
// gamemode, which preLoad does not have; the fields are what it leaves. False, and no travel this
// poll, when the level's name cannot be made yet.
bool PutLaunchLevelInForce(void* gi, void* metadata, const std::wstring& level, void* save,
                           const std::wstring& folder, const char* who) {
    if (!ue_wrap::fname_utils::Resolved()) {
        UE_LOGW("engine: %s -- the string library is not up; the level name cannot be made yet (retry)", who);
        return false;
    }
    const R::FName lv = ue_wrap::fname_utils::StringToFName(level);
    if (lv.ComparisonIndex == 0 && lv.Number == 0) {
        UE_LOGE("engine: %s -- the level name '%ls' did not convert; not travelling (retry)", who, level.c_str());
        return false;
    }
    bool curOk = false, baseOk = false;
    const R::FName curWas = ReadFNameProp(gi, P::name::GameInstanceCurrentLevelProp, curOk);
    const R::FName baseWas = ReadFNameProp(gi, P::name::GameInstanceBaseLevelProp, baseOk);
    if (!curOk) {
        // The restore cannot be made to run; travelling anyway would open a world without the save
        // that the game's next save writes over the folder.
        UE_LOGE("engine: %s -- mainGameInstance_C.%ls did not resolve; not travelling", who,
                P::name::GameInstanceCurrentLevelProp);
        return false;
    }
    WriteFNameProp(gi, P::name::GameInstanceCurrentLevelProp, lv);
    if (baseOk) WriteFNameProp(gi, P::name::GameInstanceBaseLevelProp, lv);
    if (metadata) {
        bool smOk = false;
        const R::FName sm = ReadFNameProp(metadata, P::name::SaveMetadataStarterMapProp, smOk);
        if (smOk && sm.ComparisonIndex == 0 && sm.Number == 0)
            WriteFNameProp(metadata, P::name::SaveMetadataStarterMapProp, lv);  // as trylaunch does
        WriteFNameProp(metadata, P::name::SaveMetadataCurrentLevelProp, lv);
    }
    static void* sLastSave = nullptr;
    if (save != sLastSave || !(curWas.ComparisonIndex == lv.ComparisonIndex && curWas.Number == lv.Number)) {
        UE_LOGI("engine: %s -- launch level in force: currentLevel '%ls' -> '%ls', baseLevel '%ls' -> '%ls' "
                "(save %p, folder '%ls', metadata %p)", who, FNameText(curWas).c_str(), level.c_str(),
                baseOk ? FNameText(baseWas).c_str() : L"<unresolved>", level.c_str(), save,
                folder.empty() ? L"<none>" : folder.c_str(), metadata);
    }
    sLastSave = save;
    return true;
}

// The pawn stands in the level: done once the save's restore has run there. A campaign that did
// not issue the travel, or a script gate that cannot see the restore, answers on the pawn alone as
// before; a restore not seen within the cap is reported and let through, so the boot never hangs.
bool RestoreSettled(const char* who) {
    namespace SRW = ue_wrap::save_restore_watch;
    if (!g_launchIssued) return true;
    const SRW::View v = SRW::ForCurrentWorld();
    const auto now = std::chrono::steady_clock::now();
    auto settle = [&] {
        g_launchIssued = false;
        g_restoreObservable = false;
        g_restoreWaitStart = {};
        SRW::Disarm();
        return true;
    };
    if (!v.watchLive || !g_restoreObservable) {
        UE_LOGW("engine: %s -- the save's restore cannot be observed (the loadObjects watch was not live when "
                "the level was opened); load complete on the pawn alone", who);
        return settle();
    }
    if (v.objects == SRW::Restore::Ran) {
        UE_LOGI("engine: %s -- load complete: loadObjects restored the save (currentLevel '%ls'), player data "
                "%s", who, v.currentLevel.c_str(), v.playerLoaded ? "applied (betterLoadData)" : "not seen");
        return settle();
    }
    if (v.objects == SRW::Restore::Skipped) {
        UE_LOGE("engine: %s -- the level opened but the game skipped the save's restore (currentLevel '%ls'); "
                "the world shows none of the save", who, v.currentLevel.c_str());
        return settle();
    }
    if (g_restoreWaitStart.time_since_epoch().count() == 0) {
        g_restoreWaitStart = now;
        UE_LOGI("engine: %s -- the pawn is in the level; waiting for the save's restore (loadObjects)", who);
    }
    if (now - g_restoreWaitStart > kRestoreWaitCap) {
        UE_LOGE("engine: %s -- no loadObjects seen %lld s after the pawn appeared; load called complete "
                "without it", who, static_cast<long long>(kRestoreWaitCap.count()));
        return settle();
    }
    return false;
}

// The travel, once the level state is in force: the console open of the level, re-issued each poll
// because one fired during preLoad is dropped.
void IssueOpen(const std::wstring& level, const char* who, const std::wstring& what) {
    std::wstring openCmd = L"open " + level;
    UE_LOGI("engine: %s -- at preLoad/menu; (re)issuing '%ls' (%ls)", who, openCmd.c_str(), what.c_str());
    ExecuteConsoleCommand(openCmd.c_str());
    g_launchIssued = true;
    g_restoreObservable = ue_wrap::save_restore_watch::ForCurrentWorld().watchLive;
}

// The ui_saveSlots_C CDO and its getSavePrefix, cached. The widget loads on the first menu or
// gameplay transition; before that this returns false and the caller retries. getSavePrefix is
// pure, so the CDO is a valid call target.
bool ResolveSavePrefixFn() {
    // A BP class, its CDO and its UFunctions can be collected with the menu world; a later campaign
    // re-resolves the reloaded class instead of calling into freed memory.
    if (g_saveSlotsUiCdo && !R::IsLiveByIndex(g_saveSlotsUiCdo, g_saveSlotsUiCdoIdx)) {
        UE_LOGI("engine: ResolveSavePrefixFn -- ui_saveSlots_C CDO was GC'd; re-resolving");
        g_saveSlotsUiCdo = nullptr;
        g_saveSlotsUiCdoIdx = -1;
        g_getSavePrefixFn = nullptr;
    }
    if (!g_saveSlotsUiCdo) {
        g_saveSlotsUiCdo = R::FindClassDefaultObject(L"ui_saveSlots_C");
        g_saveSlotsUiCdoIdx = g_saveSlotsUiCdo ? R::InternalIndexOf(g_saveSlotsUiCdo) : -1;
        g_getSavePrefixFn = nullptr;  // belongs to the (possibly reloaded) class
    }
    if (g_saveSlotsUiCdo && !g_getSavePrefixFn) {
        if (void* c = R::ClassOf(g_saveSlotsUiCdo))
            g_getSavePrefixFn = R::FindFunction(c, L"getSavePrefix");
    }
    return g_saveSlotsUiCdo && g_getSavePrefixFn;
}

// The slot's game mode from its name prefix, as the menu does, written to the GameInstance.
// Shares the prefix source with the save browser. Retried each poll until the widget is loaded,
// then runs once. Game thread only.
void ApplyGameModeFromSlot(void* gi, const wchar_t* slot, int forceGameMode = -1) {
    if (g_gameModeApplied || !gi || !slot) return;
    // The save-transfer slot carries a prefix the game's mode map cannot match, and a fresh coop
    // world has no slot file at all; both name their mode outright instead.
    if (forceGameMode >= 0) {
        g_gameModeApplied = true;
        const int old = ue_wrap::game_mode::WriteTo(gi, forceGameMode);
        UE_LOGI("engine: ApplyGameModeFromSlot -- slot '%ls' FORCED GameMode=%d (was %d; named mode)",
                slot, forceGameMode, old);
        return;
    }
    if (!ResolveSavePrefixFn()) {
        // The widget is not loaded yet at this boot stage: retried next poll, not latched. Should
        // it never load, the warning persists and the mode stays as-is, a visible signal.
        UE_LOGW("engine: ApplyGameModeFromSlot -- ui_saveSlots_C cdo=%p getSavePrefix=%p not loaded yet; "
                "GameMode left as-is (will retry)", g_saveSlotsUiCdo, g_getSavePrefixFn);
        return;
    }
    const int bestMode = DeriveModeFromSlot(slot);
    // The widget is loaded and getSavePrefix is deterministic, so this latches whether or not a
    // prefix matched; a re-run would give the same answer.
    g_gameModeApplied = true;
    if (bestMode >= 0) {
        const int old = ue_wrap::game_mode::WriteTo(gi, bestMode);
        UE_LOGI("engine: ApplyGameModeFromSlot -- slot '%ls' prefix-matched GameMode=%d (was %d); "
                "the mode a load outside the slot menu would leave at the engine default",
                slot, bestMode, old);
    } else {
        UE_LOGW("engine: ApplyGameModeFromSlot -- NO getSavePrefix prefix matched slot '%ls' "
                "(GameMode stays %d)", slot, ue_wrap::game_mode::ReadFrom(gi));
    }
}

// The campaign scope of the cache, one owner: called at the top of the boot phase with the target
// slot and the current non-gameplay world. A changed polling world or slot is a new campaign and
// resets everything, reloading from disk since an autosave may have rewritten the slot. Within a
// campaign a purge between polls drops only the object; the mode latch stays, the GameInstance
// already holding the byte.
void ValidateCachedSaveForCampaign(const wchar_t* slot, void* curWorld) {
    const bool worldChanged =
        g_campaignWorld &&
        (g_campaignWorld != curWorld || !R::IsLiveByIndex(g_campaignWorld, g_campaignWorldIdx));
    const bool slotChanged = !g_storySaveSlot.empty() && g_storySaveSlot != slot;
    if (worldChanged || slotChanged) {
        UE_LOGI("engine: save cache -- NEW load campaign (target '%ls', cached '%ls'%s%s) -> "
                "full reset, (re)load from disk",
                slot, g_storySaveSlot.c_str(),
                worldChanged ? ", polling world changed" : "",
                slotChanged ? ", slot changed" : "");
        ResetCachedSave();
    }
    if (!g_campaignWorld && curWorld) {
        g_campaignWorld = curWorld;
        g_campaignWorldIdx = R::InternalIndexOf(curWorld);
    }
    const bool metaDead = g_storyMeta && !R::IsLiveByIndex(g_storyMeta, g_storyMetaIdx);
    const bool persistDead = g_storyPersist && !R::IsLiveByIndex(g_storyPersist, g_storyPersistIdx);
    if (g_storySave && (!R::IsLiveByIndex(g_storySave, g_storySaveIdx) || metaDead || persistDead)) {
        UE_LOGW("engine: save cache -- cached save %p ('%ls') or a companion object was GC-purged "
                "mid-campaign; reloading from disk", g_storySave, g_storySaveSlot.c_str());
        g_storySave = nullptr;
        g_storySaveIdx = -1;
        g_storyMeta = nullptr;
        g_storyMetaIdx = -1;
        g_storyPersist = nullptr;
        g_storyPersistIdx = -1;
    }
}

// mainGameInstance_C::setSaveSlotObject with all five of its arguments, as the game's save menu
// calls it: the save, the entry's slot name, the world folder, the metadata and the persistent
// object. A flat slot has no folder and no companions; the function stores whatever it is given.
void RegisterSave(void* gi, void* save, const std::wstring& slotName, const std::wstring& folder,
                  void* metadata, void* persistent, const char* who) {
    if (!g_setSaveSlotFn) {
        if (void* gicls = R::ClassOf(gi)) g_setSaveSlotFn = R::FindFunction(gicls, P::name::SetSaveSlotObjectFn);
    }
    if (!g_setSaveSlotFn) {
        UE_LOGW("engine: %s -- setSaveSlotObject unresolved", who);
        return;
    }
    std::wstring sb = slotName, fb = folder;
    R::FString sfs{sb.data(), static_cast<int32_t>(sb.size()) + 1, static_cast<int32_t>(sb.size()) + 1};
    R::FString ffs{fb.data(), static_cast<int32_t>(fb.size()) + 1, static_cast<int32_t>(fb.size()) + 1};
    ParamFrame f(g_setSaveSlotFn);
    f.Set<void*>(L"save_gameInst", save);
    f.SetRaw(L"SlotName", &sfs, sizeof(sfs));
    f.SetRaw(L"folderSlotName", &ffs, sizeof(ffs));
    f.Set<void*>(L"metadataObject", metadata);
    f.Set<void*>(L"save_persistent", persistent);
    Call(gi, f);
}

// True when the slot's file on disk is the 0.10.0 compressed format rather than GVAS: the stale
// fallback of a host streams its world's main.sav as it is.
bool SlotFileIsCompressed(const std::wstring& path) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    uint8_t head[4] = {};
    if (!f.read(reinterpret_cast<char*>(head), 4)) return false;
    return ue_wrap::world_meta::IsCompressed(head, 4);
}
// The boot poll's two "where are we?" reads, one owner for LoadStorySave and StartFreshGame.
// Adopted from archhn0madd's Multifoid fork; see docs/credits.md.
// After a quit to the menu the dying gameplay world and its player corpse stay in GUObjectArray for
// tens of seconds and their kill flags can lag, so a class search found the corpse off the origin
// and answered "in gameplay" while the process sat at the menu. This keys on world identity
// instead: the current world comes from the GameInstance's local player chain, which a dying world
// cannot hold alive, and the pawn must be live and belong to it. The legacy first-World scan is
// kept only for the mid-travel window, where that chain cannot answer.
struct BootWorldView {
    bool inGameplay;       // (a): a LIVE mainPlayer_C of the CURRENT world, off-origin
    bool gameplayLoading;  // (b): a gameplay world is up/loading -> never re-open, wait
    void* curWorld;        // the world phase (c) stamps as the campaign's polling world
};
BootWorldView SurveyBootWorld(const char* who) {
    BootWorldView v{false, false, nullptr};
    const bool readerUp = !ue_wrap::world_identity::Degraded();
    v.curWorld = readerUp ? ue_wrap::world_identity::CurrentWorld() : nullptr;

    // (a) Already in gameplay: a mainPlayer_C placed in the level, off the origin.
    if (void* lp = R::FindObjectByClass(P::name::MainPlayerClass)) {
        const bool pawnLive = R::IsLive(lp);  // fresh-pointer guard BEFORE the WorldOf deref
        void* pawnWorld = (pawnLive && readerUp) ? ue_wrap::world_identity::WorldOf(lp) : nullptr;
        const bool currentWorldPawn =
            pawnLive && (!readerUp || (pawnWorld != nullptr && pawnWorld == v.curWorld));
        if (currentWorldPawn) {
            FVector p{};
            if (TryGetActorLocation(lp, p) && std::abs(p.X) + std::abs(p.Y) + std::abs(p.Z) > 100.f) {
                UE_LOGI("engine: %s -- in gameplay (mainPlayer @ %.0f,%.0f,%.0f)",
                        who, p.X, p.Y, p.Z);
                v.inGameplay = true;
                return v;
            }
            // A live current-world pawn at the origin, or one whose location cannot be read, is not
            // yet placed: fall through to (b).
        } else {
            // Throttled: the poll re-runs every second or so and hits the same corpse every time,
            // so the first three and then a periodic heartbeat. Keyed on the corpse's identity, not
            // a bare count: this static is shared by both callers and outlives the cache reset, so
            // a plain counter would mute the second stale corpse of a process, which in the rejoin
            // case is the one that matters.
            static void* sLastStaleActor = nullptr;
            static unsigned sStaleSkips = 0;
            if (lp != sLastStaleActor) {
                sLastStaleActor = lp;
                sStaleSkips = 0;
            }
            ++sStaleSkips;
            if (sStaleSkips <= 3 || sStaleSkips % 16 == 0) {
                UE_LOGW("engine: %s -- mainPlayer_C %p is a STALE other-world actor "
                        "(live=%d itsWorld=%p current=%p readerUp=%d) -- the quit-to-menu "
                        "corpse window; ignoring it, continuing the boot (skip #%u)",
                        who, lp, pawnLive ? 1 : 0, pawnWorld, v.curWorld,
                        readerUp ? 1 : 0, sStaleSkips);
            }
        }
    }
    // (b) The gameplay map already up or loading: never re-open, just wait for the player to spawn.
    const ue_wrap::world_identity::WorldKind kind =
        readerUp ? ue_wrap::world_identity::CurrentWorldKind()
                 : ue_wrap::world_identity::WorldKind::Unknown;
    if (kind == ue_wrap::world_identity::WorldKind::Gameplay) {
        v.gameplayLoading = true;
    } else if (kind == ue_wrap::world_identity::WorldKind::Unknown) {
        // Mid-travel, or a degraded chain: the legacy reader, where the dying gameplay world is the
        // right "wait" answer.
        v.curWorld = R::FindObjectByClass(P::name::WorldClass);
        if (v.curWorld &&
            R::ToString(R::NameOf(v.curWorld)).find(L"ntitled") != std::wstring::npos)
            v.gameplayLoading = true;
    }
    // The menu or preLoad: fall through, and the open is issued.
    return v;
}

// The save's world rules put in force on the GameInstance, which is where the game reads nearly
// all of them. The game's slot menu does this on its way into a world and both boots below skip
// that menu. Re-asserted on every poll like the save registration, since the process outlives a
// campaign and the game's own menu may have loaded another slot in between; it logs when a save is
// first seen and whenever the copy changed something.
void PutSaveRulesInForce(void* gi, void* save, const char* who) {
    static void* sLastSave = nullptr;
    const int changed = ue_wrap::game_rules::ApplySavedToProcess(gi, save);
    if (changed < 0) {
        UE_LOGW("engine: %s -- the save's world rules could not be put in force (struct unresolved)", who);
    } else if (changed > 0 || save != sLastSave) {
        UE_LOGI("engine: %s -- the save's world rules are in force (%d rule(s) changed on the GameInstance)",
                who, changed);
    }
    sLastSave = save;
}

// The save's day handed to the gamemode, the other thing the game's slot menu does on its way into
// a world: mainGameInstance.startDay = saveSlot.savedtime.Z. The gamemode reads it once on its
// BeginPlay, marks every scheduled event dated before that day as passed WITHOUT running it, sets
// its clock's day, and zeroes the field. With the field left at 0 that block does not run, and an
// event dated before the save's day that is missing from its passed list stays armed.
// Re-asserted on every poll (the boots stop polling once the world is loading, before BeginPlay).
void PutSaveStartDayInForce(void* gi, void* save, const char* who) {
    static void* sLastSave = nullptr;
    void* giClass = R::ClassOf(gi);
    void* saveClass = (save && R::IsLive(save)) ? R::ClassOf(save) : nullptr;
    const int32_t dayOff = giClass ? R::FindPropertyOffset(giClass, L"startDay") : -1;
    const int32_t timeOff = saveClass ? R::FindPropertyOffset(saveClass, L"savedtime") : -1;
    if (dayOff < 0 || timeOff < 0) {
        if (save != sLastSave) UE_LOGW("engine: %s -- startDay / savedtime unresolved; the save's day was not handed over", who);
        sLastSave = save;
        return;
    }
    // savedtime is an FIntVector; Z is the day.
    const int32_t day = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(save) + timeOff + 8);
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(gi) + dayOff) = day;
    if (save != sLastSave) UE_LOGI("engine: %s -- the save's day %d handed to the gamemode (startDay)", who, day);
    sLastSave = save;
}

// The pending new-game rules written into the blank save's saved copy; a rule that did not take is
// named, and the list is spent either way.
void WriteFreshGameRules(void* save) {
    namespace GR = ue_wrap::game_rules;
    int written = 0;
    for (const auto& kv : g_freshGameRules) {
        switch (GR::WriteSavedRule(save, kv.first, kv.second)) {
            case GR::WriteResult::Written: ++written; break;
            case GR::WriteResult::UnknownKey:
                UE_LOGW("engine: StartFreshGame -- new-game rule '%s' names no gameRules member", kv.first.c_str());
                break;
            case GR::WriteResult::BadValue:
                UE_LOGW("engine: StartFreshGame -- new-game rule '%s' cannot take '%s'", kv.first.c_str(),
                        kv.second.c_str());
                break;
            case GR::WriteResult::Unresolved:
                UE_LOGW("engine: StartFreshGame -- new-game rule '%s': the blank save's rules are unresolved",
                        kv.first.c_str());
                break;
        }
    }
    if (!g_freshGameRules.empty())
        UE_LOGI("engine: StartFreshGame -- %d of %zu new-game rule(s) written to the blank save", written,
                g_freshGameRules.size());
    g_freshGameRules.clear();
}

}  // namespace

void SetFreshGameRules(const std::vector<std::pair<std::string, std::string>>& rules) {
    g_freshGameRules = rules;
}

bool GetSavePrefix(uint8_t mode, std::wstring& out) {
    out.clear();
    if (!ResolveSavePrefixFn()) return false;
    ParamFrame f(g_getSavePrefixFn);
    f.Set<uint8_t>(L"Index", mode);  // TEnumAsByte<enum_gamemode::Type> = 1 byte
    if (!Call(g_saveSlotsUiCdo, f)) return false;
    const R::FString pre = f.Get<R::FString>(L"ReturnValue");
    if (pre.Data && pre.Num > 1) out.assign(pre.Data, pre.Data + (pre.Num - 1));  // Num counts the null
    return true;
}

int DeriveModeFromSlot(const wchar_t* slot) {
    if (!slot || !ResolveSavePrefixFn()) return -1;
    const std::wstring slotStr(slot);
    int    bestMode = -1;
    size_t bestLen  = 0;
    for (uint8_t mode = 0; mode < ue_wrap::game_mode::kCount; ++mode) {
        std::wstring pre;
        if (!GetSavePrefix(mode, pre)) continue;
        // The longest matching prefix wins, so one prefix that is a prefix of another (an empty
        // one, say) cannot win.
        if (!pre.empty() && slotStr.rfind(pre, 0) == 0 && pre.size() > bestLen) {
            bestLen  = pre.size();
            bestMode = static_cast<int>(mode);
        }
    }
    return bestMode;
}

// Called repeatedly by the boot loop. True only once a mainPlayer_C is in the level; while still
// at preLoad, the warning screen or the menu it re-issues the open each call, since a single
// open fired during preLoad is silently dropped. It does not re-open once the gameplay world is
// loading, which would restart the load.
// mainGameInstance_C's apply-the-save flag (loadObjectsFromSlot on 0.10.0, loadObjects before),
// resolved by reflection: the profile's 0.9.0n constant (0x229) is, on 0.10.0, inside a TArray, and setting it turned the array's Data
// pointer into 0x100, which the engine's exit purge then freed (the crash on quitting after a
// hosted or joined game). Refused, never guessed, when the property does not resolve.
void SetLoadObjects(void* gi) {
    static int32_t s_off = -2;
    if (s_off == -2) {
        void* cls = R::ClassOf(gi);
        s_off = R::FindPropertyOffset(cls, profile::name::GameInstanceLoadObjects);
        if (s_off < 0) s_off = R::FindPropertyOffset(cls, profile::name::GameInstanceLoadObjectsLegacy);
        if (s_off >= 0 && static_cast<size_t>(s_off) != P::off::mainGameInstance_loadObjects)
            UE_LOGW("engine: mainGameInstance_C apply-save flag at 0x%X, the profile says 0x%zX",
                    s_off, P::off::mainGameInstance_loadObjects);
    }
    if (s_off < 0) {
        UE_LOGE("engine: mainGameInstance_C apply-save flag unresolved -- not set; the save may not apply");
        return;
    }
    *(reinterpret_cast<uint8_t*>(gi) + s_off) = 1;
}

// LoadStorySave's step (c), shared with the pocket return (engine_save_internal.h).
bool detail::RegisterStorySaveAndOpen(const wchar_t* slot, int forceGameMode, void* curWorld,
                                      const char* who, bool* slotMissing) {
    // The campaign scope first, so a re-host never reuses the previous campaign's save object.
    ValidateCachedSaveForCampaign(slot, curWorld);
    auto makeFStr = [](std::wstring& b) {
        R::FString fs{};
        fs.Data = b.data();
        fs.Num = static_cast<int32_t>(b.size()) + 1;  // FString::Num counts the null
        fs.Max = fs.Num;
        return fs;
    };
    if (!g_storyGsCdo) g_storyGsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_storyGsCdo && !g_loadGameFn) {
        if (void* cls = R::ClassOf(g_storyGsCdo)) g_loadGameFn = R::FindFunction(cls, P::name::LoadGameFromSlotFn);
    }
    void* gi = world_singleton::GameInstance();
    if (!g_storyGsCdo || !g_loadGameFn || !gi) {
        UE_LOGW("engine: %s -- not up yet (cdo=%p fn=%p gi=%p); retry", who, g_storyGsCdo, g_loadGameFn, gi);
        return false;
    }
    // The slot loaded from disk once.
    if (!g_storySave) {
        const std::wstring dir = ue_wrap::world_folder::SaveGamesDir();
        if (!dir.empty() && SlotFileIsCompressed(dir + slot + P::name::WorldSaveExt)) {
            g_storySave = ue_wrap::world_folder::LoadCompressed(slot, dir, P::name::WorldSaveExt);
        } else {
            std::wstring b(slot);
            R::FString fs = makeFStr(b);
            ParamFrame f(g_loadGameFn);
            f.SetRaw(L"SlotName", &fs, sizeof(fs));
            f.Set<int32_t>(L"UserIndex", 0);
            if (!Call(g_storyGsCdo, f)) { UE_LOGE("engine: %s -- LoadGameFromSlot call failed", who); return false; }
            g_storySave = f.Get<void*>(L"ReturnValue");
        }
        if (!g_storySave) {
            UE_LOGW("engine: %s -- slot '%ls' missing/empty", who, slot);
            if (slotMissing) *slotMissing = true;
            return false;
        }
        g_storySaveIdx = R::InternalIndexOf(g_storySave);  // IsLiveByIndex guard for reuse
        g_storySaveSlot = slot;                            // campaign identity
        UE_LOGI("engine: %s -- loaded save '%ls' = %p (idx %d)", who, slot, g_storySave, g_storySaveIdx);
        // The save's inventory arrays are present and the world is not yet built from them: the one
        // moment a coop client can substitute its per-player inventory. Fires once.
        FireSaveObjectReadyHook(g_storySave);
    }

    // Registered on the persistent GameInstance and flagged for the game to apply on BeginPlay;
    // re-asserted each retry (no disk) so it is fresh at the travel.
    RegisterSave(gi, g_storySave, slot, std::wstring(), nullptr, nullptr, who);
    PutSaveRulesInForce(gi, g_storySave, who);
    PutSaveStartDayInForce(gi, g_storySave, who);
    SetLoadObjects(gi);

    // The game mode from the slot prefix before the travel, retried each poll until the widget is
    // loaded.
    ApplyGameModeFromSlot(gi, slot, forceGameMode);

    // The level the game's own launch names before its travel; a flat slot has no metadata and
    // starts in the gameplay level.
    const std::wstring level = P::name::GameplayLevel;
    ue_wrap::save_restore_watch::Arm(who);
    if (!PutLaunchLevelInForce(gi, nullptr, level, g_storySave, std::wstring(), who)) return false;
    IssueOpen(level, who, std::wstring(L"save '") + slot + L"' registered");
    return true;
}

bool LoadStorySave(const wchar_t* slot, int forceGameMode) {
    if (!slot || !*slot) return false;

    // The boot survey, shared with StartFreshGame.
    const BootWorldView w = SurveyBootWorld("LoadStorySave");
    if (w.inGameplay) return RestoreSettled("LoadStorySave");
    if (w.gameplayLoading) return false;  // in/loading the gameplay world -> wait, no re-open

    // (c) Still before gameplay: register the save (once per campaign) and re-issue the open.
    detail::RegisterStorySaveAndOpen(slot, forceGameMode, w.curWorld, "LoadStorySave");
    return false;  // not in gameplay yet -> caller keeps retrying
}

bool detail::PawnPlacedInCurrentWorld(const char* who) { return SurveyBootWorld(who).inGameplay; }

bool detail::RestoreSettledFor(const char* who) { return RestoreSettled(who); }

// A 0.10.0 world, loaded the way the game's save menu loads one (ui_saveSlots apply, then
// trylaunch): the entry's save, its metadata and the world's persistent object through
// LoadGameCompressed, all three registered with setSaveSlotObject so the game's own save writes
// back into this folder, the metadata's rules and mode put in force, the save's day handed over,
// and the gameplay level opened. Polled like LoadStorySave.
LoadResult LoadWorld(const std::wstring& folder, const std::wstring& entry) {
    const BootWorldView w = SurveyBootWorld("LoadWorld");
    if (w.inGameplay) return RestoreSettled("LoadWorld") ? LoadResult::InGameplay : LoadResult::Pending;
    if (w.gameplayLoading) return LoadResult::Pending;

    const std::wstring key = ue_wrap::world_folder::MakeKey(folder, entry);
    ValidateCachedSaveForCampaign(key.c_str(), w.curWorld);
    void* gi = world_singleton::GameInstance();
    if (!gi) {
        UE_LOGW("engine: LoadWorld -- no game instance yet; retry");
        return LoadResult::Pending;
    }
    if (!g_storySave) {
        ue_wrap::world_folder::Entry e;
        if (!ue_wrap::world_folder::LoadEntry(folder, entry, e)) {
            UE_LOGE("engine: LoadWorld -- world '%ls' entry '%ls' did not load (save=%p metadata=%p "
                    "persistent=%p)", folder.c_str(), entry.c_str(), e.save, e.metadata, e.persistent);
            return LoadResult::Failed;
        }
        const std::wstring area = ue_wrap::world_folder::MetadataArea(e.metadata);
        if (!area.empty()) {
            UE_LOGE("engine: LoadWorld -- entry '%ls' of '%ls' was saved inside the area '%ls', which a "
                    "hosted world cannot start in", entry.c_str(), folder.c_str(), area.c_str());
            return LoadResult::Failed;
        }
        // The game's launch opens the metadata's starterMap (None reads as untitled_1). Every part of
        // this boot and of the session knows the gameplay level only, so another starter map is refused
        // by name rather than opened half-supported.
        const std::wstring starter = ue_wrap::world_folder::MetadataStarterMap(e.metadata);
        if (!starter.empty() && !SameLevelName(starter, P::name::GameplayLevel)) {
            UE_LOGE("engine: LoadWorld -- world '%ls' starts in the level '%ls', not '%ls'; a hosted world "
                    "cannot start there", folder.c_str(), starter.c_str(), P::name::GameplayLevel);
            return LoadResult::Failed;
        }
        g_storySave = e.save;
        g_storySaveIdx = R::InternalIndexOf(e.save);
        g_storyMeta = e.metadata;
        g_storyMetaIdx = R::InternalIndexOf(e.metadata);
        g_storyPersist = e.persistent;
        g_storyPersistIdx = R::InternalIndexOf(e.persistent);
        g_storySaveSlot = key;
        FireSaveObjectReadyHook(g_storySave);
    }

    RegisterSave(gi, g_storySave, entry, folder, g_storyMeta, g_storyPersist, "LoadWorld");
    {
        static void* sLastMeta = nullptr;
        const int changed = ue_wrap::world_folder::ApplyMetadataRules(gi, g_storyMeta, g_storySave);
        if (changed < 0)
            UE_LOGW("engine: LoadWorld -- the world's rules could not be put in force (struct unresolved)");
        else if (changed > 0 || g_storyMeta != sLastMeta)
            UE_LOGI("engine: LoadWorld -- the world's rules are in force (%d rule(s) changed)", changed);
        sLastMeta = g_storyMeta;
    }
    PutSaveStartDayInForce(gi, g_storySave, "LoadWorld");
    SetLoadObjects(gi);
    const int mode = ue_wrap::world_folder::MetadataGameMode(g_storyMeta);
    ApplyGameModeFromSlot(gi, key.c_str(),
                          ue_wrap::game_mode::IsValid(mode) ? mode : ue_wrap::game_mode::kStory);

    // The save menu's launch: baseLevel = starterMap, then lib_C::loadLevel(starterMap) leaves
    // currentLevel naming it, which the new level's loadObjects requires to restore the save.
    const std::wstring level = P::name::GameplayLevel;
    ue_wrap::save_restore_watch::Arm("LoadWorld");
    if (!PutLaunchLevelInForce(gi, g_storyMeta, level, g_storySave, folder, "LoadWorld"))
        return LoadResult::Pending;
    IssueOpen(level, "LoadWorld", L"world '" + key + L"' registered");
    return LoadResult::Pending;
}

// Content invalidation: the next poll reloads the slot from disk even mid-campaign, for a slot
// file that changed under the same name (a rejoin re-downloads the host's world into the same
// slot). Staleness across campaigns is handled by the campaign identity.
void ResetCachedSave() {
    if (g_storySave) UE_LOGI("engine: ResetCachedSave -- dropping cached save %p ('%ls')",
                             g_storySave, g_storySaveSlot.c_str());
    g_storySave = nullptr;
    g_storySaveIdx = -1;
    g_storySaveSlot.clear();
    g_storyMeta = nullptr;
    g_storyMetaIdx = -1;
    g_storyPersist = nullptr;
    g_storyPersistIdx = -1;
    g_campaignWorld = nullptr;
    g_campaignWorldIdx = -1;
    g_gameModeApplied = false;
    g_launchIssued = false;
    g_restoreObservable = false;
    g_restoreWaitStart = {};
    detail::ResetReturnCampaign();
    ue_wrap::save_restore_watch::Disarm();
}

void SetSaveObjectReadyHook(SaveObjectReadyHook hook) {
    // Idempotent: the inventory layer re-calls this every pump tick, so it acts and logs only when
    // the hook changes.
    if (g_saveObjectReadyHook == hook) return;
    g_saveObjectReadyHook = hook;
    UE_LOGI("engine: SaveObjectReadyHook %s", hook ? "armed" : "disarmed");
}

// The fresh New Game boot: as LoadStorySave, with a blank save object created in memory rather
// than a disk slot. A fresh client has only the level-default props, so the host's connect
// snapshot mirrors its whole world onto it with nothing to reconcile away. Polled like
// LoadStorySave. Game thread.
bool StartFreshGame(int gameMode) {
    // The boot survey, shared with LoadStorySave.
    const BootWorldView w = SurveyBootWorld("StartFreshGame");
    if (w.inGameplay) return RestoreSettled("StartFreshGame");
    if (w.gameplayLoading) return false;  // in/loading the gameplay world -> wait, no re-open

    // (c) Still before gameplay: create the blank save, register it and travel. The campaign scope
    // first, so a fresh boot after any prior load never reuses the old save object.
    ValidateCachedSaveForCampaign(kFreshSlotName, w.curWorld);
    if (!g_storyGsCdo) g_storyGsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    void* gi = world_singleton::GameInstance();
    if (!g_storyGsCdo || !gi) {
        UE_LOGW("engine: StartFreshGame -- not up yet (cdo=%p gi=%p); retry", g_storyGsCdo, gi);
        return false;
    }
    void* gsCls = R::ClassOf(g_storyGsCdo);
    void* createFn = gsCls ? R::FindFunction(gsCls, L"CreateSaveGameObject") : nullptr;
    void* saveCls  = R::FindClass(L"saveSlot_C");
    if (!createFn || !saveCls) {
        UE_LOGW("engine: StartFreshGame -- CreateSaveGameObject=%p saveSlot_C=%p not resolved; retry", createFn, saveCls);
        return false;
    }
    // The blank save created once per campaign, cached like the loaded one.
    if (!g_storySave) {
        ParamFrame f(createFn);
        f.Set<void*>(L"SaveGameClass", saveCls);
        if (!Call(g_storyGsCdo, f)) { UE_LOGE("engine: StartFreshGame -- CreateSaveGameObject call failed"); return false; }
        g_storySave = f.Get<void*>(L"ReturnValue");
        if (!g_storySave) { UE_LOGW("engine: StartFreshGame -- CreateSaveGameObject returned null"); return false; }
        g_storySaveIdx = R::InternalIndexOf(g_storySave);  // IsLiveByIndex guard for reuse
        g_storySaveSlot = kFreshSlotName;                  // campaign identity
        UE_LOGI("engine: StartFreshGame -- created BLANK saveSlot_C = %p (idx %d, fresh New Game baseline)",
                g_storySave, g_storySaveIdx);
        WriteFreshGameRules(g_storySave);
        // A fresh client join still gets its per-player inventory applied onto the blank save
        // before loadObjects materialises it.
        FireSaveObjectReadyHook(g_storySave);
    } else if (!g_freshGameRules.empty()) {
        UE_LOGW("engine: StartFreshGame -- %zu new-game rule(s) named but the blank save already exists; "
                "not written", g_freshGameRules.size());
        g_freshGameRules.clear();
    }

    // The blank save registered under the pseudo-slot name.
    const wchar_t* freshSlot = kFreshSlotName;
    RegisterSave(gi, g_storySave, freshSlot, std::wstring(), nullptr, nullptr, "StartFreshGame");
    PutSaveRulesInForce(gi, g_storySave, "StartFreshGame");
    PutSaveStartDayInForce(gi, g_storySave, "StartFreshGame");
    // A blank save has empty object and trigger arrays, so restoring it yields the level defaults
    // through the same load path as a real save.
    SetLoadObjects(gi);
    // The mode outright: the pseudo-slot has no file and so no name for a prefix to be read from,
    // and every mode has to be reachable here, not the two a story-or-sandbox flag could spell.
    int mode = gameMode;
    if (!ue_wrap::game_mode::IsValid(mode)) {
        UE_LOGE("engine: StartFreshGame -- %d names no game mode; opening in story", gameMode);
        mode = ue_wrap::game_mode::kStory;
    }
    ApplyGameModeFromSlot(gi, kFreshSlotName, mode);

    // A new world launches as a loaded one does (the game's create pane hands it to the same
    // trylaunch): the level named, so loadObjects runs over the blank save and the player's own data
    // (a client's substituted inventory) is applied.
    const std::wstring level = P::name::GameplayLevel;
    ue_wrap::save_restore_watch::Arm("StartFreshGame");
    if (!PutLaunchLevelInForce(gi, nullptr, level, g_storySave, std::wstring(), "StartFreshGame")) return false;
    std::wstring what = L"BLANK save registered, mode=" + std::to_wstring(mode);
    IssueOpen(level, "StartFreshGame", what);
    return false;  // not in gameplay yet -> caller keeps retrying
}

// Travel to the main menu through the game's own level-travel verb, mainGamemode_C::transition
// with the full package path (the short name does not resolve). A direct gamemode call, so a
// dead or ragdolling player can travel with no pause menu; with the ProcessEvent detour in
// bypass for the duration it reaches the menu and tears the world down without our layer
// hanging the teardown. Game thread only.
bool ReturnToMainMenu() {
    void* gm = world_singleton::Gamemode();
    if (!gm) {
        UE_LOGW("engine: ReturnToMainMenu -- no live mainGamemode_C");
        return false;
    }
    void* fn = R::FindFunction(R::ClassOf(gm), P::name::MainGamemodeTransitionFn);
    if (!fn) {
        UE_LOGW("engine: ReturnToMainMenu -- mainGamemode_C::transition UFunction not resolved");
        return false;
    }
    R::FName ln = ue_wrap::fname_utils::StringToFName(L"/Game/menu");
    if (ln.ComparisonIndex == 0 && ln.Number == 0) {
        // StringToFName failed and returned None; a None level name is not passed to transition.
        UE_LOGW("engine: ReturnToMainMenu -- StringToFName(\"/Game/menu\") returned NAME_None; abort");
        return false;
    }
    ParamFrame f(fn);
    if (!f.valid() || !f.SetRaw(L"LevelName", &ln, sizeof(ln))) {
        UE_LOGW("engine: ReturnToMainMenu -- transition SetRaw(LevelName) failed");
        return false;
    }
    const bool ok = Call(gm, f);
    UE_LOGI("engine: ReturnToMainMenu -- mainGamemode_C::transition(\"/Game/menu\") dispatched=%d",
            ok ? 1 : 0);
    return ok;
}

}  // namespace ue_wrap::engine

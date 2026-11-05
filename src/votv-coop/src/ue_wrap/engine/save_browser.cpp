// ue_wrap/engine/save_browser.cpp -- see ue_wrap/engine/save_browser.h.

#include "ue_wrap/engine/save_browser.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"        // GetSavePrefix / DeriveModeFromSlot (legacy rows)
#include "ue_wrap/engine/gvas_meta.h"     // legacy flat slots
#include "ue_wrap/engine/world_folder.h"
#include "ue_wrap/engine/world_meta.h"
#include "ue_wrap/world/game_mode.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ue_wrap::save_browser {
namespace {

namespace R = reflection;
namespace GT = game_thread;
namespace N = profile::name;
namespace fs = std::filesystem;

std::wstring FStrToW(const R::FString& s) {
    if (s.Data && s.Num > 1) return std::wstring(s.Data, s.Data + (s.Num - 1));
    return std::wstring();
}

R::FString MakeFStr(std::wstring& buf) {
    R::FString fs{};
    fs.Data = buf.data();
    fs.Num  = static_cast<int32_t>(buf.size()) + 1;
    fs.Max  = fs.Num;
    return fs;
}

// lib_C::gameVersion(prefix, suffix) with both empty: the project version, the string the game's
// slot menu stamps on a new save and compares a loaded one against. The body is pure and never
// reads __WorldContext, so the CDO is a valid target. Game thread.
bool CallGameVersion(R::FString& out) {
    out = R::FString{};
    void* libCdo = R::FindClassDefaultObject(L"lib_C");
    void* libCls = libCdo ? R::ClassOf(libCdo) : nullptr;
    void* verFn = libCls ? R::FindFunction(libCls, L"GameVersion") : nullptr;
    if (!verFn) return false;
    ParamFrame f(verFn);
    f.Set<void*>(L"__WorldContext", libCdo);
    if (!Call(libCdo, f)) return false;
    return f.GetRaw(L"Version", &out, static_cast<int32_t>(sizeof(out))) && out.Data;
}

std::mutex   g_versionMu;
std::wstring g_gameVersion;

std::wstring ResolveGameVersion() {
    {
        std::lock_guard<std::mutex> lk(g_versionMu);
        if (!g_gameVersion.empty()) return g_gameVersion;
    }
    R::FString ver{};
    if (!CallGameVersion(ver)) {
        UE_LOGW("save_browser: lib_C.gameVersion unresolved -- no world is flagged as a version conflict");
        return {};
    }
    const std::wstring out = FStrToW(ver);
    R::EngineFree(ver.Data);  // ours: read once, kept as a copy
    UE_LOGI("save_browser: the running game's version is '%ls'", out.c_str());
    std::lock_guard<std::mutex> lk(g_versionMu);
    g_gameVersion = out;
    return out;
}

// VOTV's own subsave classifier (lib_C CDO; pure string logic). False on a resolve failure, so a
// slot is listed rather than hidden.
bool IsSubsaveName(const std::wstring& slot) {
    void* lib = R::FindClassDefaultObject(L"lib_C");
    void* fn = lib ? R::FindFunction(R::ClassOf(lib), L"processSaveNameIntoSubsave") : nullptr;
    if (!fn) return false;
    std::wstring buf = slot;
    R::FString fs = MakeFStr(buf);
    ParamFrame f(fn);
    f.SetRaw(L"saveSlotName", &fs, static_cast<int32_t>(sizeof(fs)));
    f.Set<void*>(L"__WorldContext", lib);
    if (!Call(lib, f)) return false;
    return f.Get<bool>(L"isSubsave");
}

// What one scan item is: a world folder (both entries are read from it) or a legacy flat slot.
struct ScanItem {
    bool         legacy = false;
    std::wstring name;   // the folder, or the slot
    std::wstring path;   // the folder's path with a trailing slash, or the .sav's
    int          legacyMode = -1;
    std::wstring legacyDisplay;
};

// The game's defaults the files omit (a tagged stream leaves out a value equal to its class default).
struct Defaults {
    int          metaMode = 0;        // save_metadata_C.gamemode_type
    std::wstring gameVersion;         // the running game's; empty = unresolved
};

int ByteDefault(const wchar_t* cls, const wchar_t* prop) {
    void* cdo = R::FindClassDefaultObject(cls);
    const int32_t off = cdo ? R::FindPropertyOffset(R::ClassOf(cdo), prop) : -1;
    return off >= 0 ? *(reinterpret_cast<const uint8_t*>(cdo) + off) : 0;
}

// The game-thread half. False = the save system is not resolvable yet.
bool BuildScanList(std::vector<ScanItem>& items, Defaults& def) {
    items.clear();
    const std::wstring dir = world_folder::SaveGamesDir();
    if (dir.empty()) return false;
    def.metaMode = ByteDefault(N::SaveMetadataClass, N::SaveMetadataGameModeProp);
    def.gameVersion = ResolveGameVersion();

    std::error_code ec;
    for (fs::directory_iterator it{fs::path(dir), ec}, end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) {
            // A world is a folder the game made: it holds main.meta.
            if (!fs::is_regular_file(it->path() / (std::wstring(N::WorldEntryMain) + N::WorldMetaExt), e2))
                continue;
            ScanItem item;
            item.name = it->path().filename().wstring();
            item.path = it->path().wstring() + L"/";
            items.push_back(std::move(item));
            continue;
        }
        if (!it->is_regular_file(e2)) continue;
        std::wstring ext = it->path().extension().wstring();
        for (wchar_t& c : ext) c = static_cast<wchar_t>(::towlower(c));
        if (ext != L".sav") continue;
        const std::wstring slot = it->path().stem().wstring();
        if (slot.empty() || IsSubsaveName(slot)) continue;
        ScanItem item;
        item.legacy = true;
        item.name = slot;
        item.path = it->path().wstring();
        item.legacyMode = engine::DeriveModeFromSlot(slot.c_str());
        std::wstring prefix;
        item.legacyDisplay = slot;
        if (item.legacyMode >= 0 && engine::GetSavePrefix(static_cast<uint8_t>(item.legacyMode), prefix) &&
            !prefix.empty() && slot.rfind(prefix, 0) == 0)
            item.legacyDisplay = slot.substr(prefix.size());
        items.push_back(std::move(item));
    }
    if (ec) {
        UE_LOGW("save_browser: SaveGames listing failed (%s)", ec.message().c_str());
        return false;
    }
    return true;
}

// The cache, by file, so an unchanged file is not re-read on every open.
struct CachedRow {
    int64_t  mtime = 0;
    uint64_t size = 0;
    bool     ok = false;
    SaveInfo info;
};
std::mutex g_metaMu;
std::map<std::wstring, CachedRow> g_metaCache;

bool Stat(const fs::path& p, int64_t& mtime, uint64_t& size) {
    std::error_code ec;
    size = fs::file_size(p, ec);
    if (ec) return false;
    mtime = fs::last_write_time(p, ec).time_since_epoch().count();
    return !ec;
}

void FillCommon(SaveInfo& info, const Defaults& def) {
    const wchar_t* label = info.mode >= 0 ? game_mode::Name(info.mode) : nullptr;
    info.modeLabel = label ? label : L"";
    // The game's compare is FString ==, which ignores case. An unresolved game version flags nothing.
    info.versionConflict = !def.gameVersion.empty() &&
                           ::_wcsicmp(info.version.c_str(), def.gameVersion.c_str()) != 0;
}

// One world entry's row from its .meta, or false when the entry has never been saved (a fresh
// world's autosave is a blank placeholder, and the game lists no row for it either).
bool ReadEntry(const ScanItem& it, const wchar_t* entry, const Defaults& def, SaveInfo& out, int64_t& mtime) {
    const fs::path meta = fs::path(it.path) / (std::wstring(entry) + N::WorldMetaExt);
    uint64_t size = 0;
    if (!Stat(meta, mtime, size)) return false;
    const std::wstring key = meta.wstring();
    {
        std::lock_guard<std::mutex> lk(g_metaMu);
        auto c = g_metaCache.find(key);
        if (c != g_metaCache.end() && c->second.mtime == mtime && c->second.size == size) {
            out = c->second.info;
            return c->second.ok;
        }
    }
    world_meta::WorldMeta m;
    const bool isMain = std::wstring(entry) == N::WorldEntryMain;
    bool ok = world_meta::ReadWorldMeta(key, m) && m.isMetadataClass && (isMain || m.hasLastSaved);
    SaveInfo info;
    if (ok) {
        info.folder = it.name;
        info.entry = entry;
        info.world = world_folder::MakeKey(it.name, entry);
        info.displayName = isMain ? it.name : it.name + L" (autosave)";
        info.inArea = !m.area.empty();
        info.mode = m.hasGameMode ? m.gameMode : def.metaMode;
        info.day = (m.hasSavedTimeZ ? m.savedTimeZ : 0) + 1;
        info.version = m.version;
        info.lastPlayedTicks = m.hasLastSaved ? m.lastSavedTicks : 0;
        FillCommon(info, def);
    } else if (isMain) {
        UE_LOGW("save_browser: world '%ls' has a main.meta this build cannot read -- not listed", it.name.c_str());
    }
    std::lock_guard<std::mutex> lk(g_metaMu);
    g_metaCache[key] = {mtime, size, ok, info};
    out = info;
    return ok;
}

bool ReadLegacy(const ScanItem& it, const Defaults& def, SaveInfo& out, int64_t& mtime) {
    uint64_t size = 0;
    if (!Stat(it.path, mtime, size)) return false;
    {
        std::lock_guard<std::mutex> lk(g_metaMu);
        auto c = g_metaCache.find(it.path);
        if (c != g_metaCache.end() && c->second.mtime == mtime && c->second.size == size) {
            out = c->second.info;
            return c->second.ok;
        }
    }
    gvas_meta::GvasSlotMeta m;
    const bool ok = gvas_meta::ReadSlotMeta(it.path, m) && m.isSaveSlotClass;
    SaveInfo info;
    if (ok) {
        info.legacy = true;
        info.world = it.name;
        info.displayName = it.legacyDisplay;
        info.mode = it.legacyMode;
        info.day = (m.hasSavedTimeZ ? m.savedTimeZ : 0) + 1;
        info.version = m.hasVersion ? m.version : std::wstring();
        info.lastPlayedTicks = m.hasLastSavedDate ? m.lastSavedDateTicks : 0;
        FillCommon(info, def);
    }
    std::lock_guard<std::mutex> lk(g_metaMu);
    g_metaCache[it.path] = {mtime, size, ok, info};
    out = info;
    return ok;
}

// The worker half: pure file I/O. Worlds first, newest-first; legacy slots after them, also
// newest-first, since they are shown only to say where the player's old saves went.
void ParseScanList(const std::vector<ScanItem>& items, const Defaults& def, std::vector<SaveInfo>& out) {
    struct Row { int64_t mtime; SaveInfo info; };
    std::vector<Row> rows;
    rows.reserve(items.size() * 2);
    for (const ScanItem& it : items) {
        SaveInfo info;
        int64_t mtime = 0;
        if (it.legacy) {
            if (ReadLegacy(it, def, info, mtime)) rows.push_back({mtime, std::move(info)});
            continue;
        }
        if (ReadEntry(it, N::WorldEntryMain, def, info, mtime)) rows.push_back({mtime, std::move(info)});
        if (ReadEntry(it, N::WorldEntryAutosave, def, info, mtime)) rows.push_back({mtime, std::move(info)});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.info.legacy != b.info.legacy) return !a.info.legacy;
        return a.mtime > b.mtime;
    });
    out.clear();
    out.reserve(rows.size());
    for (Row& r : rows) {
        UE_LOGI("save_browser:   '%ls'%s mode=%d(%ls) day=%d ver='%ls'%s", r.info.world.c_str(),
                r.info.legacy ? " [legacy]" : "", r.info.mode, r.info.modeLabel.c_str(), r.info.day,
                r.info.version.c_str(), r.info.inArea ? " [in an area]" : "");
        out.push_back(std::move(r.info));
    }
}

std::mutex g_mu;
std::vector<SaveInfo> g_cache;
uint64_t g_rev = 0;
std::string g_status = "No world scan yet";
std::atomic<bool> g_scanning{false};

}  // namespace

bool EnumerateSaves(std::vector<SaveInfo>& out) {
    out.clear();
    std::vector<ScanItem> items;
    Defaults def;
    if (!BuildScanList(items, def)) return false;
    ParseScanList(items, def, out);
    return true;
}

void RefreshAsync() {
    if (g_scanning.exchange(true)) return;  // a scan is already in flight
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_status = "Scanning worlds...";
    }
    GT::Post([] {
        auto items = std::make_shared<std::vector<ScanItem>>();
        auto def = std::make_shared<Defaults>();
        if (!BuildScanList(*items, *def)) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_status = "Save system not ready (try again)";
            ++g_rev;
            g_scanning.store(false, std::memory_order_release);
            return;
        }
        std::thread([items, def] {
            std::vector<SaveInfo> v;
            ParseScanList(*items, *def, v);
            std::lock_guard<std::mutex> lk(g_mu);
            g_cache.swap(v);
            size_t worlds = 0;
            for (const SaveInfo& s : g_cache) worlds += s.legacy ? 0 : 1;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%zu world%s", worlds, worlds == 1 ? "" : "s");
            g_status = buf;
            ++g_rev;
            g_scanning.store(false, std::memory_order_release);  // inside the lock
        }).detach();
    });
}

uint64_t CopySaves(std::vector<SaveInfo>& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_cache;
    return g_rev;
}

std::wstring GameVersion() {
    std::lock_guard<std::mutex> lk(g_versionMu);
    return g_gameVersion;
}

std::string Status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_status;
}

}  // namespace ue_wrap::save_browser

// ue_wrap/engine/world_folder.cpp -- see ue_wrap/engine/world_folder.h.

#include "ue_wrap/engine/world_folder.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/world/game_mode.h"
#include "ue_wrap/world/game_rules.h"
#include "ue_wrap/world/world_singleton.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace ue_wrap::world_folder {
namespace {

namespace R = reflection;
namespace N = profile::name;

R::FString AliasFStr(std::wstring& buf) {
    R::FString fs{};
    fs.Data = buf.data();
    fs.Num = static_cast<int32_t>(buf.size()) + 1;
    fs.Max = fs.Num;
    return fs;
}

std::wstring FStrToW(const R::FString& s) {
    if (s.Data && s.Num > 1) return std::wstring(s.Data, s.Data + (s.Num - 1));
    return {};
}

std::mutex   g_dirMu;
std::wstring g_dir;

// The plugin's CDO and one of its functions.
void* LibFn(const wchar_t* name, void*& cdo) {
    cdo = R::FindClassDefaultObject(N::CompressedSaveLibClass);
    return cdo ? R::FindFunction(R::ClassOf(cdo), name) : nullptr;
}

// The non-return parameters of `fn` in declaration order, and the return value's name.
std::vector<R::ParamInfo> InputsOf(void* fn, std::wstring& retName) {
    std::vector<R::ParamInfo> in;
    for (const R::ParamInfo& p : R::FunctionParams(fn)) {
        if (p.flags & profile::cpf::ReturnParm) retName = p.name;
        else in.push_back(p);
    }
    return in;
}

// The game's file-name rule for a typed world name (BlueprintPathsLibrary::MakeValidFileName with
// an empty replacement): the characters Windows refuses are dropped.
std::wstring ValidFileName(const std::wstring& name) {
    std::wstring out;
    for (wchar_t c : name) {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) continue;
        out.push_back(c);
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    return out;
}

void SaveSelf(void* obj) {
    if (!obj || !R::IsLive(obj)) return;
    if (void* fn = R::FindFunction(R::ClassOf(obj), N::SaveSelfFn)) {
        ParamFrame f(fn);
        Call(obj, f);
    }
}

}  // namespace

std::wstring SaveGamesDir() {
    {
        std::lock_guard<std::mutex> lk(g_dirMu);
        if (!g_dir.empty()) return g_dir;
    }
    void* ksl = R::FindClassDefaultObject(L"KismetSystemLibrary");
    void* fn = ksl ? R::FindFunction(R::ClassOf(ksl), L"GetProjectSavedDirectory") : nullptr;
    if (!fn) return {};
    ParamFrame f(fn);
    if (!Call(ksl, f)) return {};
    std::wstring dir = FStrToW(f.Get<R::FString>(L"ReturnValue"));
    if (dir.empty()) return {};
    if (dir.back() != L'/' && dir.back() != L'\\') dir += L'/';
    dir += L"SaveGames/";
    UE_LOGI("world_folder: SaveGames dir = '%ls'", dir.c_str());
    std::lock_guard<std::mutex> lk(g_dirMu);
    g_dir = dir;
    return g_dir;
}

std::wstring MakeKey(const std::wstring& folder, const std::wstring& entry) {
    if (entry.empty() || entry == N::WorldEntryMain) return folder;
    return folder + L"/" + entry;
}

bool SplitKey(const std::wstring& key, std::wstring& folder, std::wstring& entry) {
    const size_t slash = key.find(L'/');
    folder = key.substr(0, slash);
    entry = slash == std::wstring::npos ? std::wstring(N::WorldEntryMain) : key.substr(slash + 1);
    return !folder.empty() && (entry == N::WorldEntryMain || entry == N::WorldEntryAutosave);
}

bool Exists(const std::wstring& folder) {
    std::wstring dir;
    {
        std::lock_guard<std::mutex> lk(g_dirMu);
        dir = g_dir;
    }
    if (dir.empty() || folder.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_directory(std::filesystem::path(dir + folder), ec);
}

void* LoadCompressed(const std::wstring& name, const std::wstring& dir, const std::wstring& ext) {
    void* cdo = nullptr;
    void* fn = LibFn(N::LoadGameCompressedFn, cdo);
    if (!fn) {
        UE_LOGW("world_folder: %ls.%ls unresolved", N::CompressedSaveLibClass, N::LoadGameCompressedFn);
        return nullptr;
    }
    std::wstring retName;
    const std::vector<R::ParamInfo> in = InputsOf(fn, retName);
    if (in.size() != 4 || retName.empty()) {
        UE_LOGE("world_folder: %ls takes %zu parameter(s), not (name, path, ext, userIndex) -- not called",
                N::LoadGameCompressedFn, in.size());
        return nullptr;
    }
    std::wstring b0 = name, b1 = dir, b2 = ext;
    R::FString s0 = AliasFStr(b0), s1 = AliasFStr(b1), s2 = AliasFStr(b2);
    ParamFrame f(fn);
    f.SetRaw(in[0].name.c_str(), &s0, sizeof(s0));
    f.SetRaw(in[1].name.c_str(), &s1, sizeof(s1));
    f.SetRaw(in[2].name.c_str(), &s2, sizeof(s2));
    f.Set<int32_t>(in[3].name.c_str(), 0);
    if (!Call(cdo, f)) return nullptr;
    return f.Get<void*>(retName.c_str());
}

bool LoadEntry(const std::wstring& folder, const std::wstring& entry, Entry& out) {
    out = Entry{};
    const std::wstring root = SaveGamesDir();
    if (root.empty()) return false;
    const std::wstring dir = root + folder;
    out.save = LoadCompressed(entry, dir, N::WorldSaveExt);
    out.metadata = LoadCompressed(entry, dir, N::WorldMetaExt);
    out.persistent = LoadCompressed(N::WorldPersistentName, dir, N::WorldSaveExt);
    UE_LOGI("world_folder: loaded '%ls' entry '%ls' -- save=%p metadata=%p persistent=%p", folder.c_str(),
            entry.c_str(), out.save, out.metadata, out.persistent);
    // The game's own launch refuses a world without its save or its persistent file; the metadata
    // carries the rules and the mode, so it is required too.
    return out.save && out.metadata && out.persistent;
}

int MetadataGameMode(void* metadata) {
    if (!metadata || !R::IsLive(metadata)) return -1;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(metadata), N::SaveMetadataGameModeProp);
    if (off < 0) return -1;
    return *(reinterpret_cast<const uint8_t*>(metadata) + off);
}

std::wstring MetadataArea(void* metadata) {
    if (!metadata || !R::IsLive(metadata)) return {};
    const int32_t off = R::FindPropertyOffset(R::ClassOf(metadata), N::SaveMetadataAreaProp);
    if (off < 0) return {};
    return FStrToW(*reinterpret_cast<const R::FString*>(reinterpret_cast<const uint8_t*>(metadata) + off));
}

std::wstring MetadataStarterMap(void* metadata) {
    if (!metadata || !R::IsLive(metadata)) return {};
    const int32_t off = R::FindPropertyOffset(R::ClassOf(metadata), N::SaveMetadataStarterMapProp);
    if (off < 0) return {};
    const R::FName n = *reinterpret_cast<const R::FName*>(reinterpret_cast<const uint8_t*>(metadata) + off);
    if (n.ComparisonIndex == 0 && n.Number == 0) return {};  // None
    return R::ToString(n);
}

std::wstring SaveObjectFile(void* saveSlot, const wchar_t* entry) {
    if (!saveSlot || !R::IsLive(saveSlot)) return {};
    void* cls = R::ClassOf(saveSlot);
    const int32_t pathOff = R::FindPropertyOffset(cls, N::SaveSlotSavePathProp);
    const int32_t nameOff = R::FindPropertyOffset(cls, N::SaveSlotSlotNameProp);
    if (pathOff < 0 || nameOff < 0) return {};
    const auto* base = reinterpret_cast<const uint8_t*>(saveSlot);
    const std::wstring dir = FStrToW(*reinterpret_cast<const R::FString*>(base + pathOff));
    const std::wstring name = FStrToW(*reinterpret_cast<const R::FString*>(base + nameOff));
    if (dir.empty() || name.empty()) return {};
    std::filesystem::path p(dir);
    if (p.is_relative()) p = std::filesystem::path(SaveGamesDir()) / p;
    // The object's slotName is its own entry (main). An autosave is written beside it, into the
    // same folder, under the entry's name, by the same saveToSlot call.
    p /= (entry && *entry ? std::wstring(entry) : name) + N::WorldSaveExt;
    return p.make_preferred().wstring();
}

int ApplyMetadataRules(void* gameInstance, void* metadata, void* save) {
    const int changed = game_rules::CopyRules(gameInstance, N::GameInstanceGameRulesProp, metadata,
                                              N::SaveMetadataRulesProp);
    if (save && game_rules::CopyRules(save, N::SaveSlotLocalRulesProp, metadata, N::SaveMetadataRulesProp) < 0)
        UE_LOGW("world_folder: the save object's %ls did not take the metadata's rules", N::SaveSlotLocalRulesProp);
    return changed;
}

bool Create(const std::wstring& name, uint8_t mode, std::wstring& outFolder) {
    outFolder.clear();
    const std::wstring folder = ValidFileName(name);
    if (folder.empty()) {
        UE_LOGW("world_folder: Create -- '%ls' is empty as a file name", name.c_str());
        return false;
    }
    if (SaveGamesDir().empty()) return false;
    if (Exists(folder)) {
        UE_LOGW("world_folder: Create -- a world '%ls' already exists", folder.c_str());
        return false;
    }
    void* gi = world_singleton::GameInstance();
    void* lib = R::FindClassDefaultObject(L"lib_C");
    void* fn = lib ? R::FindFunction(R::ClassOf(lib), N::CreateSaveSlotTreeFn) : nullptr;
    if (!gi || !fn) {
        UE_LOGW("world_folder: Create -- game instance %p or lib_C.%ls %p unresolved", gi,
                N::CreateSaveSlotTreeFn, fn);
        return false;
    }
    // The game copies the mode off the game instance into the new metadata, as its menu does after
    // the mode was picked.
    game_mode::WriteTo(gi, mode);
    std::wstring buf = folder;
    R::FString fs = AliasFStr(buf);
    ParamFrame f(fn);
    f.SetRaw(L"slotName", &fs, sizeof(fs));
    f.Set<void*>(L"__WorldContext", gi);
    if (!Call(lib, f) || !f.Get<bool>(L"return")) {
        UE_LOGW("world_folder: Create -- %ls('%ls') refused", N::CreateSaveSlotTreeFn, folder.c_str());
        return false;
    }
    // The tree function already wrote every file; the menu then saves the main entry once more
    // after stamping the starter map, the rules and the start day, which a silent create leaves at
    // the game's defaults.
    SaveSelf(f.Get<void*>(L"saveSlot_main"));
    SaveSelf(f.Get<void*>(L"metadata_main"));
    if (!Exists(folder)) {
        UE_LOGE("world_folder: Create -- the game reported '%ls' made but no folder is on disk", folder.c_str());
        return false;
    }
    outFolder = folder;
    UE_LOGI("world_folder: created world '%ls' (mode %u)", folder.c_str(), static_cast<unsigned>(mode));
    return true;
}

bool CreateUnique(const std::wstring& baseName, uint8_t mode, std::wstring& outFolder) {
    outFolder.clear();
    const std::wstring base = ValidFileName(baseName);
    if (base.empty() || SaveGamesDir().empty()) return false;
    // Numbered from two, the way a save list numbers a repeat; the cap refuses rather than reuse.
    for (int n = 1; n <= 99; ++n) {
        const std::wstring name = n > 1 ? base + L" " + std::to_wstring(n) : base;
        if (Exists(name)) continue;
        return Create(name, mode, outFolder);
    }
    UE_LOGW("world_folder: CreateUnique -- '%ls' and 98 numbered variants all exist", base.c_str());
    return false;
}

}  // namespace ue_wrap::world_folder

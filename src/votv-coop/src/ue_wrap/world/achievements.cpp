// ue_wrap/world/achievements.cpp -- see ue_wrap/world/achievements.h.

#include "ue_wrap/world/achievements.h"

#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/data_table.h"
#include "ue_wrap/world/game_mode.h"
#include "ue_wrap/world/profile.h"
#include "ue_wrap/world/votv_lib.h"
#include "ue_wrap/world/world_singleton.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace ue_wrap::achievements {
namespace {

namespace R  = reflection;
namespace DT = data_table;
namespace N  = ue_wrap::profile::name;

constexpr int kDomains = 2;

int Slot(Domain d) { return static_cast<int>(d); }

const char* DomainName(Domain d) { return d == Domain::Achievement ? "achievement" : "advancement"; }

// A TArray's header: the data, then its count and capacity.
struct ArrayHeader {
    uint8_t* data;
    int32_t  num;
    int32_t  max;
};

// Where a store keeps its three arrays and the members of its row struct, resolved from the live class once and
// latched either way: a class that lacks a member lacks it for as long as that class lives, so the lookup does
// not repeat and the miss is said once. The class is remembered so a store of another class resolves afresh.
struct Layout {
    void*   cls = nullptr;
    int32_t names = -1;      // TArray<FName>
    int32_t completed = -1;  // TArray<bool>
    int32_t rows = -1;       // TArray<struct_achievement>
    int32_t stride = -1;     // the row array's element size
    int32_t progress = -1;   // a row's progress, int32
    int32_t target = -1;     // a row's progressTarget, int32
    bool    missing = false;
};
Layout g_layout[kDomains];

// A catalogue's rows by lower-cased name, each with its progressTarget, read once the table has loaded: it is a
// cooked asset that does not change.
struct Catalogue {
    bool loaded = false;
    bool structural = false;
    bool saidAbsent = false;
    std::unordered_map<std::string, int32_t> targets;
};
Catalogue g_catalogue[kDomains];

bool g_saidSkipped[kDomains] = {false, false};

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// A row name as the ASCII the wire carries, or false for one that is empty, too long or not printable ASCII.
bool Narrow(const std::wstring& w, std::string& out) {
    if (w.empty() || w.size() > kNameCap) return false;
    out.clear();
    for (wchar_t c : w) {
        if (c < 0x21 || c > 0x7e) return false;
        out.push_back(static_cast<char>(c));
    }
    return true;
}

void* StoreOf(Domain d) { return d == Domain::Achievement ? profile::Save() : world_singleton::SaveSlot(); }

const wchar_t* NamesProp(Domain d) { return d == Domain::Achievement ? N::AchievementNamesProp : N::AdvancementNamesProp; }
const wchar_t* RowsProp(Domain d) { return d == Domain::Achievement ? N::AchievementRowsProp : N::AdvancementRowsProp; }
const wchar_t* CompletedProp(Domain d) {
    return d == Domain::Achievement ? N::AchievementCompletedProp : N::AdvancementCompletedProp;
}
const wchar_t* CatalogueName(Domain d) {
    return d == Domain::Achievement ? N::AchievementCatalogue : N::AdvancementCatalogue;
}

// The layout of `store`, or null while a member does not resolve.
const Layout* LayoutOf(Domain d, void* store) {
    Layout& l = g_layout[Slot(d)];
    void* cls = R::ClassOf(store);
    if (l.cls != cls) {
        l = Layout{};
        l.cls = cls;
    }
    if (l.missing) return nullptr;
    if (l.stride >= 0) return &l;
    R::ArrayInnerInfo inner;
    l.names = R::FindPropertyOffset(cls, NamesProp(d));
    l.completed = R::FindPropertyOffset(cls, CompletedProp(d));
    l.rows = R::FindPropertyOffset(cls, RowsProp(d));
    const bool haveInner = l.rows >= 0 && R::ArrayPropertyInner(cls, RowsProp(d), inner) && inner.innerStruct &&
                           inner.elementSize > 0;
    l.progress = haveInner ? R::FindPropertyOffsetByPrefix(inner.innerStruct, N::AchievementProgressMember) : -1;
    l.target = haveInner ? R::FindPropertyOffsetByPrefix(inner.innerStruct, N::AchievementTargetMember) : -1;
    if (l.names < 0 || l.completed < 0 || l.rows < 0 || !haveInner || l.progress < 0 || l.target < 0) {
        l.missing = true;
        UE_LOGW("achievements: the %s store (%ls) does not resolve (names@%d completed@%d rows@%d progress@%d "
                "target@%d) -- out of reach", DomainName(d), R::ClassNameOf(store).c_str(), l.names, l.completed,
                l.rows, l.progress, l.target);
        return nullptr;
    }
    l.stride = inner.elementSize;
    return &l;
}

const ArrayHeader& HeaderAt(void* store, int32_t off) {
    return *reinterpret_cast<const ArrayHeader*>(static_cast<uint8_t*>(store) + off);
}

// The arrays are parallel; a row is addressable only where all three have it.
int32_t RowCount(void* store, const Layout& l) {
    return std::min({HeaderAt(store, l.names).num, HeaderAt(store, l.completed).num, HeaderAt(store, l.rows).num});
}

// The index of the row named `name`, or -1.
int32_t IndexOf(void* store, const Layout& l, const std::wstring& name) {
    const ArrayHeader& names = HeaderAt(store, l.names);
    if (!names.data) return -1;
    const int32_t n = RowCount(store, l);
    const auto* at = reinterpret_cast<const R::FName*>(names.data);
    for (int32_t i = 0; i < n; ++i)
        if (R::NameEquals(at[i], name.c_str())) return i;
    return -1;
}

bool CompletedAt(void* store, const Layout& l, int32_t idx) {
    const ArrayHeader& done = HeaderAt(store, l.completed);
    return done.data && idx >= 0 && idx < done.num && done.data[idx] != 0;
}

int32_t* IntAt(void* store, const Layout& l, int32_t idx, int32_t member) {
    const ArrayHeader& rows = HeaderAt(store, l.rows);
    if (!rows.data || idx < 0 || idx >= rows.num) return nullptr;
    return reinterpret_cast<int32_t*>(rows.data + static_cast<size_t>(idx) * static_cast<size_t>(l.stride) + member);
}

// Read the catalogue once its table has loaded.
void LoadCatalogue(Domain d) {
    Catalogue& c = g_catalogue[Slot(d)];
    if (c.loaded || c.structural) return;
    void* table = R::FindObject(CatalogueName(d), L"DataTable");
    if (!table) {
        if (!c.saidAbsent) UE_LOGW("achievements: %ls is not loaded -- asked again at the next call", CatalogueName(d));
        c.saidAbsent = true;
        return;
    }
    void* rowStruct = DT::RowStruct(table);
    const int32_t targetOff = rowStruct ? R::FindPropertyOffsetByPrefix(rowStruct, N::AchievementTargetMember) : -1;
    std::vector<DT::RowRef> rows;
    if (targetOff < 0 || !DT::Rows(table, rows, "achievements") || rows.empty()) {
        c.structural = true;
        UE_LOGW("achievements: %ls did not resolve (target@%d, %zu rows) -- its rows cannot be checked",
                CatalogueName(d), targetOff, rows.size());
        return;
    }
    for (const DT::RowRef& r : rows) {
        std::string name;
        if (!Narrow(R::ToString(r.key), name)) continue;
        c.targets[Lower(name)] = *reinterpret_cast<const int32_t*>(r.row + targetOff);
    }
    c.loaded = true;
    UE_LOGI("achievements: %ls holds %zu rows", CatalogueName(d), c.targets.size());
}

// The catalogue row's progressTarget: 1 found, 0 not listed, -1 table not loaded, -2 layout unresolved.
int CatalogueTarget(Domain d, const std::string& name, int32_t& target) {
    LoadCatalogue(d);
    const Catalogue& c = g_catalogue[Slot(d)];
    if (c.structural) return -2;
    if (!c.loaded) return -1;
    const auto it = c.targets.find(Lower(name));
    if (it == c.targets.end()) return 0;
    target = it->second;
    return 1;
}

// The game's own progress verb, with its popup unless `popup` is false.
bool Native(Domain d, const std::wstring& name, bool popup) {
    if (d == Domain::Achievement) return profile::ProgressAchievement(name.c_str(), popup);
    return votv_lib::ProgressAdvancement(name, /*disablePopup=*/!popup, world_singleton::Gamemode());
}

}  // namespace

const char* Describe(Applied result) {
    switch (result) {
    case Applied::Completed: return "completed";
    case Applied::AlreadyDone: return "already done";
    case Applied::NotReady: return "not ready";
    case Applied::Unknown: return "not in the catalogue";
    case Applied::Unsupported: return "not in this world";
    case Applied::Failed: return "failed";
    }
    return "?";
}

bool ReadCompleted(Domain d, std::vector<std::string>& out) {
    out.clear();
    void* store = StoreOf(d);
    const Layout* l = store ? LayoutOf(d, store) : nullptr;
    if (!l) return false;
    const ArrayHeader& names = HeaderAt(store, l->names);
    const ArrayHeader& done = HeaderAt(store, l->completed);
    const int32_t n = RowCount(store, *l);
    if (!names.data || !done.data) return true;  // an empty store
    const auto* at = reinterpret_cast<const R::FName*>(names.data);
    for (int32_t i = 0; i < n; ++i) {
        if (!done.data[i]) continue;
        std::string name;
        if (!Narrow(R::ToString(at[i]), name)) {
            if (!g_saidSkipped[Slot(d)]) UE_LOGW("achievements: a completed %s row's name is not carried (over %zu characters or not ASCII) -- skipped",
                                                 DomainName(d), kNameCap);
            g_saidSkipped[Slot(d)] = true;
            continue;
        }
        out.push_back(std::move(name));
    }
    return true;
}

int InCatalogue(Domain d, const std::string& name) {
    int32_t target = 0;
    return CatalogueTarget(d, name, target);
}

Applied Complete(Domain d, const std::string& name, bool popup) {
    void* store = StoreOf(d);
    const Layout* l = store ? LayoutOf(d, store) : nullptr;
    if (!l) return Applied::NotReady;
    if (d == Domain::Advancement) {
        const int mode = game_mode::ReadLocal();
        if (mode < 0) return Applied::NotReady;
        if (mode != 0) return Applied::Unsupported;
    }
    const std::wstring wname = Widen(name);
    int32_t idx = IndexOf(store, *l, wname);
    if (idx >= 0 && CompletedAt(store, *l, idx)) return Applied::AlreadyDone;

    // The target is the stored row's when it exists, else the catalogue's, which the native path will copy in.
    int32_t target = 1;
    if (idx >= 0) {
        const int32_t* t = IntAt(store, *l, idx, l->target);
        if (t) target = *t;
    } else {
        const int found = CatalogueTarget(d, name, target);
        if (found == 0) return Applied::Unknown;
        if (found == -1) return Applied::NotReady;
        if (found < 0) return Applied::Failed;
    }
    if (target < 1) target = 1;

    // A row not stored yet is appended by its first native progress. A counter's is made quietly, so the popup
    // that follows is the completing one; a one-step row completes in that same call.
    if (idx < 0 && target >= 2) {
        if (!Native(d, wname, /*popup=*/false)) return Applied::Failed;
        idx = IndexOf(store, *l, wname);
        if (idx < 0) return Applied::Failed;
    }
    if (idx >= 0) {
        int32_t* progress = IntAt(store, *l, idx, l->progress);
        if (progress && *progress < target - 1) *progress = target - 1;
    }
    if (!Native(d, wname, popup)) return Applied::Failed;
    idx = IndexOf(store, *l, wname);
    return idx >= 0 && CompletedAt(store, *l, idx) ? Applied::Completed : Applied::Failed;
}

}  // namespace ue_wrap::achievements

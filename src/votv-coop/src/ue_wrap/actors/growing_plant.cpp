// ue_wrap/actors/growing_plant.cpp -- see ue_wrap/actors/growing_plant.h.

#include "ue_wrap/actors/growing_plant.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflected_offset.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/data_table.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <cmath>
#include <cstring>
#include <cwchar>
#include <utility>

namespace ue_wrap::growing_plant {
namespace {

namespace R = reflection;
namespace P = profile;
namespace N = profile::name;

void* PlantBase() { return object_index::ClassByName(N::GrowingPlantClass); }
void* PotBase() { return object_index::ClassByName(N::FarmPotClass); }
void* PlayerBase() { return object_index::ClassByName(N::MainPlayerClass); }

bool Descends(void* cls, void* base) { return cls && base && R::IsDescendantOfAny(cls, &base, 1); }
bool Finite(const FVector& v) { return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z); }
bool Finite(const FRotator& r) { return std::isfinite(r.Pitch) && std::isfinite(r.Yaw) && std::isfinite(r.Roll); }

// A member of a base class, found by name and type, asked again when a new world re-creates the class. A descendant
// carries the base's members at the base's offsets, so one resolve serves every plant class.
struct Member {
    void* (*base)();
    const wchar_t* name;
    const wchar_t* type;   // FFieldClass name; "BoolProperty" takes the bit mask
    CachedObjRef   cls;
    int32_t        off = -1;
    uint8_t        mask = 0;
    bool           ok = false;
    bool           said = false;
};

bool Resolve(Member& m) {
    void* cls = m.base();
    if (!cls) return false;
    if (m.cls.Is(cls)) return m.ok;
    m.cls.Set(cls);
    m.ok = false;
    if (std::wcscmp(m.type, L"BoolProperty") == 0) {
        int32_t off = -1;
        uint8_t mask = 0;
        m.ok = R::FindBoolProperty(cls, m.name, off, mask) && off >= 0 && mask != 0;
        m.off = off;
        m.mask = mask;
    } else {
        int32_t off = -1;
        std::wstring found;
        m.ok = R::PropertyOffsetAndClass(cls, m.name, off, found) && off >= 0 && found == m.type;
        m.off = off;
    }
    if (!m.ok && !m.said) {
        m.said = true;
        UE_LOGW("growing_plant: %ls.%ls (%ls) did not resolve by name and type", R::ClassNameOf(cls).c_str(), m.name,
                m.type);
    }
    return m.ok;
}

Member g_plant{&PlantBase, N::GrowingPlantPlantProp, L"NameProperty", {}};
Member g_grow{&PlantBase, N::GrowingPlantGrowProp, L"FloatProperty", {}};
Member g_grew{&PlantBase, N::GrowingPlantGrewProp, L"BoolProperty", {}};
Member g_water{&PlantBase, N::GrowingPlantWaterProp, L"FloatProperty", {}};
Member g_dryness{&PlantBase, N::GrowingPlantDrynessProp, L"FloatProperty", {}};
Member g_fert{&PlantBase, N::GrowingPlantFertilizerProp, L"FloatProperty", {}};
Member g_fruit{&PlantBase, N::GrowingPlantFruitGrowProp, L"FloatProperty", {}};
Member g_health{&PlantBase, N::GrowingPlantHealthProp, L"FloatProperty", {}};
Member g_amount{&PlantBase, N::GrowingPlantAmountProp, L"IntProperty", {}};
Member g_disease{&PlantBase, N::GrowingPlantDiseaseNameProp, L"NameProperty", {}};
Member g_diseaseComp{&PlantBase, N::GrowingPlantDiseaseCompProp, L"ObjectProperty", {}};
Member g_discard{&PlantBase, N::GrowingPlantDiscardProp, L"BoolProperty", {}};
Member g_hideDirt{&PlantBase, N::GrowingPlantHideDirtProp, L"BoolProperty", {}};
Member g_attachTo{&PlantBase, N::GrowingPlantAttachToProp, L"ObjectProperty", {}};
Member g_data{&PlantBase, N::GrowingPlantDataProp, L"StructProperty", {}};
Member g_potPlant{&PotBase, N::FarmPotPlantProp, L"ObjectProperty", {}};
Member g_inputAlt{&PlayerBase, N::MainPlayerInputAltProp, L"BoolProperty", {}};

// The two row members the lane reads from `data`, by their stable prefix (the GUID tail is the cook's).
struct DataMembers {
    CachedObjRef cls;
    int32_t      type = -1;
    int32_t      range = -1;
};
DataMembers g_dataMembers;

bool ResolveData() {
    if (!Resolve(g_data)) return false;
    void* cls = PlantBase();
    if (g_dataMembers.cls.Is(cls)) return g_dataMembers.type >= 0 && g_dataMembers.range >= 0;
    g_dataMembers.cls.Set(cls);
    void* st = R::PropertyInnerStruct(cls, N::GrowingPlantDataProp);
    g_dataMembers.type = st ? R::FindPropertyOffsetByPrefix(st, N::FarmPlantTypeMember) : -1;
    g_dataMembers.range = st ? R::FindPropertyOffsetByPrefix(st, N::FarmPlantAmountRangeMember) : -1;
    if (g_dataMembers.type < 0 || g_dataMembers.range < 0)
        UE_LOGW("growing_plant: struct_farmPlant's plant type or amount range did not resolve -- the row reads as type 0");
    return g_dataMembers.type >= 0 && g_dataMembers.range >= 0;
}

template <class T>
bool Read(Member& m, void* obj, T& out) {
    if (!obj || !Resolve(m)) return false;
    std::memcpy(&out, reinterpret_cast<const uint8_t*>(obj) + m.off, sizeof(T));
    return true;
}
template <class T>
bool Write(Member& m, void* obj, const T& v) {
    if (!obj || !Resolve(m)) return false;
    std::memcpy(reinterpret_cast<uint8_t*>(obj) + m.off, &v, sizeof(T));
    return true;
}
bool ReadBool(Member& m, void* obj, bool& out) {
    if (!obj || !Resolve(m)) return false;
    out = (*(reinterpret_cast<const uint8_t*>(obj) + m.off) & m.mask) != 0;
    return true;
}
bool WriteBool(Member& m, void* obj, bool v) {
    if (!obj || !Resolve(m)) return false;
    uint8_t* b = reinterpret_cast<uint8_t*>(obj) + m.off;
    *b = v ? static_cast<uint8_t>(*b | m.mask) : static_cast<uint8_t>(*b & ~m.mask);
    return true;
}

std::wstring NameText(const R::FName& n) {
    if (n.ComparisonIndex == 0 && n.Number == 0) return {};
    std::wstring s = R::ToString(n);
    return s == L"None" ? std::wstring{} : s;
}

bool MakeName(const std::wstring& s, R::FName& out) {
    out = R::FName{0, 0};
    if (s.empty()) return true;
    out = fname_utils::StringToFName(s);
    return out.ComparisonIndex != 0;
}

// Call a no-argument function of the object's own class.
bool CallNoArgs(void* obj, const wchar_t* fnName) {
    if (!obj || !R::IsLive(obj)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(obj), fnName);
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(obj, f);
}

bool CallBoolArg(void* obj, const wchar_t* fnName, const wchar_t* param, bool v) {
    if (!obj || !R::IsLive(obj)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(obj), fnName);
    if (!fn) return false;
    ParamFrame f(fn);
    const uint8_t b = v ? 1 : 0;
    return f.valid() && f.SetRaw(param, &b, 1) && Call(obj, f);
}

// list_farmplants, read once it has loaded: each row's name and the three members a pot asks. The table is asset data
// and does not change while it lives.
std::vector<std::pair<std::wstring, FarmRow>> g_farmRows;
bool g_farmRowsRead = false;
bool g_farmRowsSaid = false;

bool LoadFarmRows() {
    if (g_farmRowsRead) return true;
    void* table = R::FindObject(N::FarmPlantTable, L"DataTable");
    void* st = table ? data_table::RowStruct(table) : nullptr;
    const int32_t type = st ? R::FindPropertyOffsetByPrefix(st, N::FarmPlantTypeMember) : -1;
    const int32_t pots = st ? R::FindPropertyOffsetByPrefix(st, N::FarmPlantCanGrowInPotsMember) : -1;
    const int32_t size = st ? R::FindPropertyOffsetByPrefix(st, N::FarmPlantMinSizeMember) : -1;
    std::vector<data_table::RowRef> rows;
    if (type < 0 || pots < 0 || size < 0 || !data_table::Rows(table, rows, "growing_plant")) {
        if (!g_farmRowsSaid) UE_LOGW("growing_plant: list_farmplants did not read yet -- asked again at the next pot");
        g_farmRowsSaid = true;
        return false;
    }
    g_farmRows.clear();
    for (const data_table::RowRef& r : rows) {
        FarmRow f;
        f.plantType = r.row[type];
        f.canGrowInPots = r.row[pots] != 0;
        std::memcpy(&f.minSize, r.row + size, sizeof(float));
        g_farmRows.emplace_back(R::ToString(r.key), f);
    }
    g_farmRowsRead = true;
    UE_LOGI("growing_plant: list_farmplants holds %zu rows", g_farmRows.size());
    return true;
}

void CollectClass(void* ctx, void* cls, void*) {
    auto* out = static_cast<std::vector<void*>*>(ctx);
    if (IsPlantClass(cls)) out->push_back(cls);
}

}  // namespace

bool IsPlantClass(void* cls) { return Descends(cls, PlantBase()); }
bool IsPlant(void* obj) { return obj && R::IsLive(obj) && IsPlantClass(R::ClassOf(obj)); }
bool IsStolasHerb(void* obj) {
    return obj && R::IsLive(obj) && Descends(R::ClassOf(obj), object_index::ClassByName(N::GrowingPlantStolasHerbClass));
}

std::vector<void*> AllPlants() {
    std::vector<void*> classes;
    object_index::ForEachClass(&CollectClass, &classes);
    std::vector<void*> found;
    for (void* cls : classes)
        object_index::ForEachInstance(
            cls, [](void* ctx, void* obj, int32_t) { static_cast<std::vector<void*>*>(ctx)->push_back(obj); }, &found);
    void* world = world_identity::CurrentWorld();
    std::vector<void*> out;
    out.reserve(found.size());
    for (void* obj : found) {
        if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        if (world && world_identity::WorldOf(obj) != world) continue;
        out.push_back(obj);
    }
    return out;
}

bool ReadState(void* plant, State& out) {
    if (!IsPlant(plant)) return false;
    State s;
    R::FName row{0, 0}, disease{0, 0};
    if (!Read(g_plant, plant, row) || !Read(g_grow, plant, s.grow) || !Read(g_water, plant, s.water) ||
        !Read(g_dryness, plant, s.dryness) || !Read(g_fert, plant, s.fertilizer) || !Read(g_fruit, plant, s.fruitgrow) ||
        !Read(g_health, plant, s.health) || !Read(g_amount, plant, s.amount) || !ReadBool(g_grew, plant, s.grew))
        return false;
    ReadBool(g_hideDirt, plant, s.hideDirt);
    if (Read(g_disease, plant, disease)) s.disease = NameText(disease);
    s.plant = NameText(row);
    if (ResolveData()) {
        const uint8_t* d = reinterpret_cast<const uint8_t*>(plant) + g_data.off;
        s.plantType = d[g_dataMembers.type];
        std::memcpy(&s.amountMin, d + g_dataMembers.range, sizeof(int32_t));
        std::memcpy(&s.amountMax, d + g_dataMembers.range + 4, sizeof(int32_t));
    }
    out = std::move(s);
    return true;
}

bool WriteState(void* plant, const State& in) {
    if (!IsPlant(plant)) return false;
    if (!in.plant.empty()) {
        R::FName have{0, 0};
        if (!Read(g_plant, plant, have)) return false;
        if (NameText(have) != in.plant) {
            R::FName want{0, 0};
            if (!MakeName(in.plant, want) || !Write(g_plant, plant, want)) return false;
        }
    }
    return Write(g_grow, plant, in.grow) && Write(g_water, plant, in.water) && Write(g_dryness, plant, in.dryness) &&
           Write(g_fert, plant, in.fertilizer) && Write(g_fruit, plant, in.fruitgrow) &&
           Write(g_health, plant, in.health) && Write(g_amount, plant, in.amount) &&
           WriteBool(g_hideDirt, plant, in.hideDirt);
}

bool WriteGrew(void* plant, bool grew) { return IsPlant(plant) && WriteBool(g_grew, plant, grew); }
bool ReadDiscard(void* plant, bool& discard) { return IsPlant(plant) && ReadBool(g_discard, plant, discard); }
bool WriteDiscard(void* plant, bool discard) { return IsPlant(plant) && WriteBool(g_discard, plant, discard); }

bool WriteAmountRange(void* plant, int32_t amountMin, int32_t amountMax) {
    if (!IsPlant(plant) || !ResolveData()) return false;
    uint8_t* d = reinterpret_cast<uint8_t*>(plant) + g_data.off;
    std::memcpy(d + g_dataMembers.range, &amountMin, sizeof(int32_t));
    std::memcpy(d + g_dataMembers.range + 4, &amountMax, sizeof(int32_t));
    return true;
}

bool CallUpd(void* plant, bool skipDataTable) {
    return IsPlant(plant) && CallBoolArg(plant, N::GrowingPlantUpdFn, N::GrowingPlantUpdSkipParam, skipDataTable);
}
bool CallUpdLoc(void* plant) { return IsPlant(plant) && CallNoArgs(plant, N::GrowingPlantUpdLocFn); }
bool CallProcessGrown(void* plant, bool skipGrowCheck) {
    return IsPlant(plant) &&
           CallBoolArg(plant, N::GrowingPlantProcessGrownFn, N::GrowingPlantProcessGrownParam, skipGrowCheck);
}

bool AddDisease(void* plant, const std::wstring& name, bool& success) {
    success = false;
    if (!IsPlant(plant) || name.empty()) return false;
    R::FName n{0, 0};
    if (!MakeName(name, n)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(plant), N::GrowingPlantAddDiseaseFn);
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !f.SetRaw(N::GrowingPlantAddDiseaseNameParam, &n, static_cast<int32_t>(sizeof(n))) || !Call(plant, f))
        return false;
    success = f.Get<uint8_t>(L"success") != 0;
    return true;
}

bool HealDisease(void* plant) {
    void* comp = nullptr;
    if (!IsPlant(plant) || !Read(g_diseaseComp, plant, comp) || !comp || !R::IsLive(comp)) return false;
    return CallNoArgs(comp, N::PlantDiseaseHealFn);
}

bool AddMedicine(void* plant, uint8_t type, float time) {
    if (!IsPlant(plant)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(plant), N::GrowingPlantAddMedicineFn);
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<uint8_t>(N::GrowingPlantMedicineTypeParam, type) &&
           f.Set<float>(N::GrowingPlantMedicineTimeParam, time) && Call(plant, f);
}

bool PlayHarvestSound(void* plant) { return IsPlant(plant) && CallNoArgs(plant, N::GrowingPlantHarvestSoundFn); }

bool CallHarvestVerb(void* plant, void* player) {
    FVector at{};
    if (!IsPlant(plant) || !player || !engine::TryGetActorLocation(plant, at)) return false;
    return prop_use::CallActionOptionIndex(plant, player, nullptr, at, 4, nullptr);
}

void* AttachTo(void* plant) {
    void* a = nullptr;
    if (!IsPlant(plant) || !Read(g_attachTo, plant, a) || !a || !R::IsLive(a)) return nullptr;
    return a;
}

std::wstring KeyOf(void* plant) {
    if (!IsPlant(plant)) return {};
    R::FName k{0, 0};
    std::memcpy(&k, reinterpret_cast<uint8_t*>(plant) + P::off::Aactor_save_Key, sizeof(k));
    return NameText(k);
}

bool SetKey(void* plant, const std::wstring& key) {
    if (!IsPlant(plant) || key.empty()) return false;
    R::FName k{0, 0};
    if (!MakeName(key, k)) return false;
    std::memcpy(reinterpret_cast<uint8_t*>(plant) + P::off::Aactor_save_Key, &k, sizeof(k));
    return true;
}

void* SpawnPlant(void* cls, const FVector& loc, const FRotator& rot, const std::wstring& plantName, const std::wstring& key,
                 void* attachTo, bool hideDirt) {
    if (!IsPlantClass(cls) || !Finite(loc) || !Finite(rot) || plantName.empty()) return nullptr;
    R::FName row{0, 0};
    if (!MakeName(plantName, row) || row.ComparisonIndex == 0 || !Resolve(g_plant) || !Resolve(g_attachTo)) return nullptr;
    void* a = engine::BeginDeferredSpawn(cls, loc, rot);
    if (!a) return nullptr;
    Write(g_plant, a, row);
    if (!key.empty()) SetKey(a, key);
    if (attachTo) engine::WriteObjectField(a, static_cast<size_t>(g_attachTo.off), attachTo);
    WriteBool(g_hideDirt, a, hideDirt);
    if (!engine::FinishDeferredSpawn(a, loc, rot) || !R::IsLive(a)) return nullptr;
    return a;
}

bool ReadFarmRow(const std::wstring& name, FarmRow& out) {
    if (name.empty() || !LoadFarmRows()) return false;
    for (const auto& [key, row] : g_farmRows)
        if (key.size() == name.size() && _wcsicmp(key.c_str(), name.c_str()) == 0) {
            out = row;
            return true;
        }
    return false;
}

bool PotWouldPlant(void* pot, const std::wstring& seedName) {
    FarmRow row;
    if (!IsFarmPot(pot) || PotPlant(pot) || !ReadFarmRow(seedName, row)) return false;
    return row.plantType != 2 && row.canGrowInPots && engine::GetActorScale3D(pot).X >= row.minSize;
}

bool IsFarmPot(void* obj) { return obj && R::IsLive(obj) && Descends(R::ClassOf(obj), PotBase()); }

void* PotPlant(void* pot) {
    void* p = nullptr;
    if (!IsFarmPot(pot) || !Read(g_potPlant, pot, p) || !p || !R::IsLive(p)) return nullptr;
    return p;
}

bool CallPotPlant(void* pot, void* player, void* seed, const std::wstring& seedName) {
    const int32_t off = reflected_offset::MainPlayer_holding_actor();
    FVector at{};
    R::FName name{0, 0};
    if (!IsFarmPot(pot) || !player || !seed || off < 0 || !engine::TryGetActorLocation(pot, at) || !MakeName(seedName, name))
        return false;
    void* prev = nullptr;
    std::memcpy(&prev, reinterpret_cast<uint8_t*>(player) + off, sizeof(prev));
    engine::WriteObjectField(player, static_cast<size_t>(off), seed);
    const bool ran = prop_use::CallPlayerUsedOn(pot, player, nullptr, at, nullptr, seed, name);
    engine::WriteObjectField(player, static_cast<size_t>(off), prev);
    return ran;
}

void* SpawnSeed(void* cls, const FVector& loc, const std::wstring& name) {
    R::FName row{0, 0};
    if (!cls || !prop::IsClassDescendantOfProp(cls) || !Finite(loc) || !MakeName(name, row) || row.ComparisonIndex == 0)
        return nullptr;
    void* a = engine::BeginDeferredSpawn(cls, loc, FRotator{});
    if (!a) return nullptr;
    prop::WriteSpParityIdentity(a, row, false, false, true, true);
    if (!engine::FinishDeferredSpawn(a, loc, FRotator{}) || !R::IsLive(a)) return nullptr;
    return a;
}

bool ReadAltHeld(void* player, bool& held) { return player && R::IsLive(player) && ReadBool(g_inputAlt, player, held); }

bool CallRetargetAddToInventory(void* player, void* item, bool& success) {
    success = false;
    if (!player || !R::IsLive(player) || !item || !R::IsLive(item)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(player), N::MainPlayerRetargetAddFn);
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !f.Set<void*>(N::MainPlayerRetargetItemParam, item) || !Call(player, f)) return false;
    success = f.Get<uint8_t>(L"success") != 0;
    return true;
}

}  // namespace ue_wrap::growing_plant

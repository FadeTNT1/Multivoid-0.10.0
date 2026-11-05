// ue_wrap/actors/inventory_component.cpp -- see ue_wrap/actors/inventory_component.h.

#include "ue_wrap/actors/inventory_component.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/facts/function_row.h"

#include <cstring>
#include <string>

namespace ue_wrap::inventory_component {
namespace {

namespace R = reflection;
namespace P = profile;
namespace N = profile::name;
namespace SG = script_gate;

UE_FACT_FN(kRowBetterLoadData, "player_inventory", L"mainPlayer_C", L"betterLoadData", Declared);
UE_FACT_FN(kRowLoadObjects, "player_inventory", L"mainGamemode_C", L"loadObjects", Declared);
constexpr int kTagPlayerLoaded  = 0x50494E31;  // 'PIN1'
constexpr int kTagObjectsLoaded = 0x50494E32;  // 'PIN2'

PlayerLoadedFn  g_onPlayerLoaded  = nullptr;
ObjectsLoadedFn g_onObjectsLoaded = nullptr;

// A TArray header as the engine lays it out; the element type is never read here.
struct RawArray {
    uint8_t* data;
    int32_t  num;
    int32_t  max;
};
static_assert(sizeof(RawArray) == 16, "TArray header");

// A native of ThingsAndStuffLib and its parameters in declaration order, the return excluded.
struct LibFn {
    void* fn = nullptr;
    std::vector<R::ParamInfo> params;
};

bool ResolveLibFn(const wchar_t* name, size_t arity, LibFn& out) {
    if (out.fn) return true;
    void* cls = R::FindClass(N::ThingsAndStuffLibClass);
    void* fn = cls ? R::FindFunction(cls, name) : nullptr;
    if (!fn) return false;
    std::vector<R::ParamInfo> params;
    for (const R::ParamInfo& p : R::FunctionParams(fn))
        if (!(p.flags & P::cpf::ReturnParm)) params.push_back(p);
    if (params.size() != arity) {
        UE_LOGE("inventory_component: %ls.%ls takes %zu parameters, not %zu -- the game changed "
                "its signature; the player inventory is not read or written",
                N::ThingsAndStuffLibClass, name, params.size(), arity);
        return false;
    }
    out.fn = fn;
    out.params = std::move(params);
    return true;
}

// The parameters by role, checked by size: (object, bool, out bytes) and (object, bytes, bool).
bool ObjectToBytesFn(LibFn*& out) {
    static LibFn s;
    if (!ResolveLibFn(N::ObjectToBytesFn, 3, s)) return false;
    if (s.params[0].size != 8 || s.params[1].size != 1 || s.params[2].size != 16) return false;
    out = &s;
    return true;
}

bool BytesIntoObjectFn(LibFn*& out) {
    static LibFn s;
    if (!ResolveLibFn(N::BytesIntoObjectFn, 3, s)) return false;
    if (s.params[0].size != 8 || s.params[1].size != 16 || s.params[2].size != 1) return false;
    out = &s;
    return true;
}

void* LibObject() {
    return R::FindClassDefaultObject(N::ThingsAndStuffLibClass);
}

// The offset of one of the component's item arrays, or -1.
int32_t ArrayOffset(void* comp, const wchar_t* name) {
    return comp ? R::FindPropertyOffset(R::ClassOf(comp), name) : -1;
}

constexpr const wchar_t* kItemArrays[] = {
    N::InventoryCarriedArray, N::InventoryHandsArray, N::InventoryEquipmentArray,
};

int32_t ArrayNum(void* comp, const wchar_t* name) {
    const int32_t off = ArrayOffset(comp, name);
    if (off < 0) return -1;
    RawArray a{};
    std::memcpy(&a, static_cast<uint8_t*>(comp) + off, sizeof(a));
    return a.num;
}

void OnBetterLoadDataPost(const SG::Call& c) {
    if (g_onPlayerLoaded && c.object) g_onPlayerLoaded(c.object);
}

void OnLoadObjectsPost(const SG::Call& c) {
    if (g_onObjectsLoaded && c.object) g_onObjectsLoaded(c.object);
}

}  // namespace

void* Of(void* player) {
    if (!player || !R::IsLive(player)) return nullptr;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(player), N::MainPlayerInventoryComponent);
    if (off < 0) return nullptr;
    void* comp = *reinterpret_cast<void**>(static_cast<uint8_t*>(player) + off);
    return (comp && R::IsLive(comp)) ? comp : nullptr;
}

bool Capture(void* player, std::vector<uint8_t>& out) {
    out.clear();
    void* comp = Of(player);
    LibFn* fn = nullptr;
    void* lib = LibObject();
    if (!comp || !lib || !ObjectToBytesFn(fn)) return false;
    ParamFrame f(fn->fn);
    const bool flag = false;  // the game's own save passes false
    f.SetRaw(fn->params[0].name.c_str(), &comp, 8);
    f.SetRaw(fn->params[1].name.c_str(), &flag, 1);
    if (!Call(lib, f)) return false;
    RawArray bytes{};
    f.GetRaw(fn->params[2].name.c_str(), &bytes, sizeof(bytes));
    if (bytes.data && bytes.num > 0) out.assign(bytes.data, bytes.data + bytes.num);
    if (bytes.data) R::EngineFree(bytes.data);  // the native allocated it into our frame
    return true;
}

bool Restore(void* player, const std::vector<uint8_t>& bytes) {
    void* comp = Of(player);
    if (!comp) return false;
    int32_t offs[3];
    for (int i = 0; i < 3; ++i) {
        offs[i] = ArrayOffset(comp, kItemArrays[i]);
        if (offs[i] < 0) {
            UE_LOGE("inventory_component: %ls.%ls does not resolve -- the player inventory is not "
                    "written", R::ClassNameOf(comp).c_str(), kItemArrays[i]);
            return false;
        }
    }
    LibFn* fn = nullptr;
    void* lib = LibObject();
    if (!bytes.empty() && (!lib || !BytesIntoObjectFn(fn))) return false;
    // Emptied in place: Num to zero, the buffer kept, so the load below refills the same storage.
    for (const int32_t off : offs)
        *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(comp) + off + 8) = 0;
    if (bytes.empty()) return true;
    void* buf = R::EngineAlloc(bytes.size());
    if (!buf) return false;
    std::memcpy(buf, bytes.data(), bytes.size());
    const RawArray arg{static_cast<uint8_t*>(buf), static_cast<int32_t>(bytes.size()),
                       static_cast<int32_t>(bytes.size())};
    const bool flag = false;  // the game's own load passes false
    ParamFrame f(fn->fn);
    f.SetRaw(fn->params[0].name.c_str(), &comp, 8);
    f.SetRaw(fn->params[1].name.c_str(), &arg, sizeof(arg));
    f.SetRaw(fn->params[2].name.c_str(), &flag, 1);
    const bool ok = Call(lib, f);
    R::EngineFree(buf);  // a const reference parameter: the native reads it and keeps nothing
    return ok;
}

std::wstring HeldItemKey(void* holdingProp) {
    if (!holdingProp || !R::IsLive(holdingProp)) return {};
    void* propCls = R::FindClass(N::PropClass);
    if (!propCls || !R::IsDescendantOfAny(R::ClassOf(holdingProp), &propCls, 1)) return {};
    const int32_t off = R::FindPropertyOffset(propCls, N::PropItemKeyProp);
    if (off < 0) return {};
    R::FName n{};
    std::memcpy(&n, static_cast<const uint8_t*>(holdingProp) + off, sizeof(n));
    std::wstring s = R::ToString(n);
    if (s == L"None") s.clear();
    return s;
}

Counts CountItems(void* player) {
    Counts c;
    void* comp = Of(player);
    if (!comp) return c;
    c.carried = ArrayNum(comp, N::InventoryCarriedArray);
    c.hands = ArrayNum(comp, N::InventoryHandsArray);
    c.equipment = ArrayNum(comp, N::InventoryEquipmentArray);
    return c;
}

bool CallUpdateEquipment(void* player) {
    if (!player || !R::IsLive(player)) return false;
    void* fn = R::FindFunction(R::ClassOf(player), N::MainPlayerUpdateEquipmentFn);
    if (!fn) return false;
    ParamFrame f(fn);
    return Call(player, f);
}

bool ArmStartupKit(void* saveSlot) {
    if (!saveSlot || !R::IsLive(saveSlot)) return false;
    void* cls = R::ClassOf(saveSlot);
    void* gs = R::FindClassDefaultObject(N::GameplayStaticsClass);
    void* createFn = gs ? R::FindFunction(R::ClassOf(gs), N::CreateSaveGameObjectFn) : nullptr;
    void* resetFn = R::FindFunction(cls, N::SaveSlotResetPlayerFn);
    int32_t mapOff = -1, mapSize = -1;
    for (const R::StructFieldInfo& fi : R::EnumerateStructFields(cls))
        if (::_wcsicmp(fi.name.c_str(), N::SaveSlotStartupEquipment) == 0) {
            mapOff = fi.offset;
            mapSize = fi.size;
        }
    if (!createFn || !resetFn || mapOff < 0 || mapSize <= 0) {
        UE_LOGE("inventory_component: the starting kit cannot be armed (create=%p reset_player=%p "
                "startupEquipment@%d) -- a first join starts with nothing", createFn, resetFn, mapOff);
        return false;
    }
    // The scratch save object is born blank (no objects, so reset_player clears nothing) and left
    // to the collector, holding whatever map the join save object held.
    ParamFrame create(createFn);
    create.Set<void*>(L"SaveGameClass", cls);
    if (!Call(gs, create)) return false;
    void* scratch = create.Get<void*>(L"ReturnValue");
    if (!scratch || !R::IsLive(scratch)) return false;
    ParamFrame reset(resetFn);
    if (!Call(scratch, reset)) return false;
    // A TMap is relocatable: the engine moves containers by swapping their bytes.
    auto* a = static_cast<uint8_t*>(saveSlot) + mapOff;
    auto* b = static_cast<uint8_t*>(scratch) + mapOff;
    std::vector<uint8_t> tmp(a, a + mapSize);
    std::memcpy(a, b, static_cast<size_t>(mapSize));
    std::memcpy(b, tmp.data(), static_cast<size_t>(mapSize));
    return true;
}

bool InstallLoadWatches(PlayerLoadedFn onPlayerLoaded, ObjectsLoadedFn onObjectsLoaded) {
    g_onPlayerLoaded = onPlayerLoaded;
    g_onObjectsLoaded = onObjectsLoaded;
    static bool s_player = false, s_objects = false;
    if (!s_player) s_player = SG::WatchName(kRowBetterLoadData, kTagPlayerLoaded, nullptr, &OnBetterLoadDataPost);
    if (!s_objects) s_objects = SG::WatchName(kRowLoadObjects, kTagObjectsLoaded, nullptr, &OnLoadObjectsPost);
    if (s_player && s_objects) SG::ResolvePendingNames();
    return s_player && s_objects;
}

}  // namespace ue_wrap::inventory_component

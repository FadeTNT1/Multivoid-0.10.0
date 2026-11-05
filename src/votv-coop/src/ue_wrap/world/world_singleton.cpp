// ue_wrap/world/world_singleton.cpp -- the world's one-of-a-kind objects, found through the object
// index. See world_singleton.h.
#include "ue_wrap/world/world_singleton.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/world_instances.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::world_singleton {
namespace {

struct Pick {
    CachedObjRef* ref;
    bool actor;
    void* world;  // the running world, or null when it cannot be told (boot, mid-travel)
};

// Into `ref`, the newest instance of the loaded class named `className` that world_instances::IsTakeable
// admits: live and readable, not its default object, and of the running world. Null, with the ref
// reset, when the class is not loaded or has no such instance.
void* Resolve(const wchar_t* className, CachedObjRef& ref) {
    ref.Reset();
    void* cls = object_index::ClassByName(className);
    if (!cls) return nullptr;
    Pick pick{&ref, world_instances::IsActorClass(cls), world_identity::CurrentWorld()};
    object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t index) {
        Pick& p = *static_cast<Pick*>(ctx);
        if (p.ref->Raw()) return;
        // Every instance that has a world is judged by it, not only an actor: the held reference rejects
        // one of another world on its first Get, so taking it would hide a later instance of the
        // running one (the menu's own UWorld read as absent behind another world's).
        if (!world_instances::IsTakeable(obj, index, p.actor, p.world)) return;
        p.ref->Set(obj);
    }, &pick);
    return ref.Get();
}

void* Cached(CachedObjRef& ref, const wchar_t* className) {
    if (void* live = ref.Get()) return live;
    return Resolve(className, ref);
}

// The tail of named singletons, a dozen at most, looked up by name.
struct Entry {
    std::wstring name;
    CachedObjRef inst;
};
std::vector<Entry> g_entries;   // game thread

}  // namespace

void* Find(const wchar_t* className) {
    UE_ASSERT_GAME_THREAD("world_singleton::Find");
    if (!className || !*className) return nullptr;
    for (Entry& e : g_entries)
        if (e.name == className) return Cached(e.inst, className);
    g_entries.push_back(Entry{className, {}});
    return Cached(g_entries.back().inst, className);
}

void* Gamemode() {
    UE_ASSERT_GAME_THREAD("world_singleton::Gamemode");
    static CachedObjRef s_ref;
    return Cached(s_ref, profile::name::GamemodeClass);
}

void* GameInstance() {
    UE_ASSERT_GAME_THREAD("world_singleton::GameInstance");
    static CachedObjRef s_ref;
    return Cached(s_ref, profile::name::GameInstanceClass);
}

int32_t GamemodeSaveSlotOffset(void* gm) {
    return GamemodeSaveSlotOffsetOnClass(gm ? reflection::ClassOf(gm) : nullptr);
}

int32_t GamemodeSaveSlotOffsetOnClass(void* cls) {
    if (!cls) return -1;
    static void* s_cls = nullptr;
    static int32_t s_off = -1;
    if (cls == s_cls) return s_off;
    int32_t off = reflection::FindPropertyOffset(cls, profile::name::GamemodeSaveSlotProp);
    if (off < 0) off = reflection::FindPropertyOffset(cls, profile::name::GamemodeSaveSlotPropLegacy);
    s_cls = cls;
    s_off = off;
    if (off < 0)
        UE_LOGW("world_singleton: %ls declares neither '%ls' nor '%ls' -- no save-object reader can "
                "reach the save", reflection::ToString(reflection::NameOf(cls)).c_str(), profile::name::GamemodeSaveSlotProp,
                profile::name::GamemodeSaveSlotPropLegacy);
    else
        UE_LOGI("world_singleton: the gamemode's save object is at +0x%X", off);
    return off;
}

void* SaveSlot() {
    void* gm = Gamemode();
    const int32_t off = GamemodeSaveSlotOffset(gm);
    if (off < 0) return nullptr;
    void* save = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + off);
    return (save && reflection::IsLive(save)) ? save : nullptr;
}

}  // namespace ue_wrap::world_singleton

// ue_wrap/actors/shrimp_tray.cpp -- see ue_wrap/actors/shrimp_tray.h.

#include "ue_wrap/actors/shrimp_tray.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstring>
#include <string>

namespace ue_wrap::shrimp_tray {
namespace {

namespace R = reflection;
namespace P = profile;

// A member of prop_food_shrimp_C, found on the class by name and checked by type; asked again when a new world
// re-creates the class.
struct Member {
    const wchar_t* name;
    const wchar_t* type;
    CachedObjRef   cls;
    int32_t        off = -1;
    bool           ok = false;
    bool           said = false;
};
Member g_count{P::name::ShrimpTrayCountProp, L"IntProperty", {}};
Member g_drop{P::name::ShrimpTrayDropClassProp, L"ClassProperty", {}};

void* TrayClass() { return object_index::ClassByName(P::name::ShrimpTrayClass); }

bool Resolve(Member& m) {
    void* cls = TrayClass();
    if (!cls) return false;
    if (m.cls.Is(cls)) return m.ok;
    m.cls.Set(cls);
    int32_t off = -1;
    std::wstring found;
    m.ok = R::PropertyOffsetAndClass(cls, m.name, off, found) && off >= 0 && found == m.type;
    m.off = m.ok ? off : -1;
    if (!m.ok && !m.said) {
        m.said = true;
        UE_LOGW("shrimp_tray: %ls.%ls (%ls) did not resolve by name and type (found '%ls')", P::name::ShrimpTrayClass,
                m.name, m.type, found.c_str());
    }
    return m.ok;
}

}  // namespace

bool IsTray(void* obj) {
    if (!obj || !R::IsLive(obj)) return false;
    void* base = TrayClass();
    void* cls = R::ClassOf(obj);
    return base && cls && R::IsDescendantOfAny(cls, &base, 1);
}

bool ReadCount(void* tray, int32_t& out) {
    if (!IsTray(tray) || !Resolve(g_count)) return false;
    std::memcpy(&out, reinterpret_cast<const uint8_t*>(tray) + g_count.off, sizeof(out));
    return true;
}

bool WriteCountAndRedraw(void* tray, int32_t count) {
    if (!IsTray(tray) || !Resolve(g_count)) return false;
    std::memcpy(reinterpret_cast<uint8_t*>(tray) + g_count.off, &count, sizeof(count));
    if (void* fn = R::FindDispatchFunctionCached(R::ClassOf(tray), P::name::ShrimpTrayUpdFn)) {
        ParamFrame f(fn);
        if (f.valid()) Call(tray, f);
    }
    return true;
}

void* DropClass(void* tray) {
    if (!IsTray(tray) || !Resolve(g_drop)) return nullptr;
    void* cls = nullptr;
    std::memcpy(&cls, reinterpret_cast<const uint8_t*>(tray) + g_drop.off, sizeof(cls));
    return cls && R::IsLive(cls) ? cls : nullptr;
}

bool CallDispenseAction(void* tray, void* player, uint8_t action) {
    if (!IsTray(tray)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(tray), L"actionOptionIndex");
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && f.Set<uint8_t>(L"action", action) && Call(tray, f);
}

bool CallHandUse(void* tray, void* player) {
    if (!IsTray(tray)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(tray), P::name::HandUseRmbFn);
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && Call(tray, f);
}

}  // namespace ue_wrap::shrimp_tray

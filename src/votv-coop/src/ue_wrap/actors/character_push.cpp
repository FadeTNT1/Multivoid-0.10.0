// ue_wrap/actors/character_push.cpp -- see ue_wrap/actors/character_push.h.

#include "ue_wrap/actors/character_push.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace ue_wrap::character_push {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

namespace {

// Raw engine memory at a byte offset. A negative offset (a property that did not resolve) is refused.
template <class T>
T ReadAt(void* base, ptrdiff_t off) {
    if (!base || off < 0) return T{};
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off);
}
template <class T>
void WriteAt(void* base, ptrdiff_t off, T value) {
    if (!base || off < 0) return;
    *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off) = value;
}

// The resolved layout of one movement-component class: float offsets by name (-1 when missing) and the
// bool's byte and mask. Kept for the last class seen, re-resolved when a pawn of another class arrives.
struct Layout {
    void*   cls = nullptr;
    int32_t push = -1, initialPush = -1, touch = -1, standing = -1;
    int32_t boolByte = -1;
    uint8_t boolMask = 0;
};
Layout g_layout;

// One float property's offset, or -1. Says once per class, with the class name, when the property is missing or is
// not a FloatProperty, since a double would be read wrong.
int32_t FloatOffset(void* cls, const wchar_t* name) {
    int32_t off = -1;
    std::wstring type;
    if (!R::PropertyOffsetAndClass(cls, name, off, type) || off < 0) {
        UE_LOGW("character_push: %ls not found on %ls -- that force is left alone", name, R::ClassNameOf(cls).c_str());
        return -1;
    }
    if (type != L"FloatProperty") {
        UE_LOGW("character_push: %ls on %ls is a %ls, not a FloatProperty -- left alone", name,
                R::ClassNameOf(cls).c_str(), type.c_str());
        return -1;
    }
    return off;
}

void* MovementOf(void* pawn) {
    if (!pawn || !R::IsLive(pawn)) return nullptr;
    void* cmc = ReadAt<void*>(pawn, P::off::ACharacter_CharacterMovement);
    return (cmc && R::IsLive(cmc)) ? cmc : nullptr;
}

const Layout& LayoutFor(void* cmc) {
    void* cls = R::ClassOf(cmc);
    if (cls == g_layout.cls) return g_layout;
    g_layout = Layout{};
    g_layout.cls = cls;
    g_layout.push        = FloatOffset(cls, P::name::CmcPushForceFactorProp);
    g_layout.initialPush = FloatOffset(cls, P::name::CmcInitialPushForceFactorProp);
    g_layout.touch       = FloatOffset(cls, P::name::CmcTouchForceFactorProp);
    g_layout.standing    = FloatOffset(cls, P::name::CmcStandingDownwardForceScaleProp);
    int32_t byteOff = -1;
    uint8_t mask = 0;
    if (R::FindBoolProperty(cls, P::name::CmcEnablePhysicsInteractionProp, byteOff, mask) && byteOff >= 0 && mask) {
        g_layout.boolByte = byteOff;
        g_layout.boolMask = mask;
    } else {
        UE_LOGW("character_push: bEnablePhysicsInteraction not found on %ls", R::ClassNameOf(cls).c_str());
    }
    return g_layout;
}

}  // namespace

bool Read(void* pawn, PushValues& out) {
    UE_ASSERT_GAME_THREAD("character_push::Read");
    void* cmc = MovementOf(pawn);
    if (!cmc) return false;
    const Layout& l = LayoutFor(cmc);
    PushValues v;
    if (l.boolByte >= 0) {
        v.haveInteraction = true;
        v.interaction = (ReadAt<uint8_t>(cmc, l.boolByte) & l.boolMask) != 0;
    }
    if (l.push >= 0)        { v.havePush = true;        v.push = ReadAt<float>(cmc, l.push); }
    if (l.initialPush >= 0) { v.haveInitialPush = true; v.initialPush = ReadAt<float>(cmc, l.initialPush); }
    if (l.touch >= 0)       { v.haveTouch = true;       v.touch = ReadAt<float>(cmc, l.touch); }
    if (l.standing >= 0)    { v.haveStanding = true;    v.standing = ReadAt<float>(cmc, l.standing); }
    if (!(v.haveInteraction || v.havePush || v.haveInitialPush || v.haveTouch || v.haveStanding)) return false;
    out = v;
    return true;
}

bool WriteForces(void* pawn, const PushValues& v) {
    UE_ASSERT_GAME_THREAD("character_push::WriteForces");
    void* cmc = MovementOf(pawn);
    if (!cmc) return false;
    const Layout& l = LayoutFor(cmc);
    if (v.havePush && l.push >= 0)               WriteAt<float>(cmc, l.push, v.push);
    if (v.haveInitialPush && l.initialPush >= 0) WriteAt<float>(cmc, l.initialPush, v.initialPush);
    if (v.haveTouch && l.touch >= 0)             WriteAt<float>(cmc, l.touch, v.touch);
    if (v.haveStanding && l.standing >= 0)       WriteAt<float>(cmc, l.standing, v.standing);
    return true;
}

void* MovementBaseOf(void* pawn) {
    UE_ASSERT_GAME_THREAD("character_push::MovementBaseOf");
    if (!pawn || !R::IsLive(pawn)) return nullptr;
    static void*   s_cls = nullptr;
    static int32_t s_off = -1;
    void* cls = R::ClassOf(pawn);
    if (cls != s_cls) {
        s_cls = cls;
        s_off = -1;
        int32_t off = -1;
        std::wstring type;
        if (R::PropertyOffsetAndClass(cls, P::name::CharacterBasedMovementProp, off, type) && off >= 0 &&
            type == L"StructProperty") {
            s_off = off;
        } else {
            UE_LOGW("character_push: BasedMovement not found as a StructProperty on %ls -- no movement base is read",
                    R::ClassNameOf(cls).c_str());
        }
    }
    if (s_off < 0) return nullptr;
    void* base = ReadAt<void*>(pawn, s_off + static_cast<ptrdiff_t>(P::off::FBasedMovementInfo_MovementBase));
    return (base && R::IsLive(base)) ? base : nullptr;
}

}  // namespace ue_wrap::character_push

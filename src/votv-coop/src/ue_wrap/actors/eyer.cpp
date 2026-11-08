// ue_wrap/actors/eyer.cpp -- see ue_wrap/actors/eyer.h.

#include "ue_wrap/actors/eyer.h"

#include "ue_wrap/core/reflection.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::eyer {

namespace R = reflection;

bool IsAngry(void* eyer) {
    if (!eyer || !R::IsLive(eyer)) return false;
    // The member's place is kept per class object and found again when another class object comes (a reloaded world).
    static void*   cls = nullptr;
    static int32_t off = -1;
    static uint8_t mask = 0;
    void* const c = R::ClassOf(eyer);
    if (c != cls) {
        cls = c;
        off = -1;
        mask = 0;
        if (!R::FindBoolProperty(c, L"isAngry", off, mask)) off = -1;
    }
    return off >= 0 && (*(static_cast<const uint8_t*>(eyer) + off) & mask) != 0;
}

void* OverlapOtherActor(const script_gate::Call& call) {
    static R::ParamOffset otherActor{L"OtherActor"};
    const int32_t off = otherActor.Of(call.function);
    if (off < 0 || !call.locals) return nullptr;
    void* actor = nullptr;
    std::memcpy(&actor, call.locals + off, sizeof(actor));
    return actor;
}

}  // namespace ue_wrap::eyer

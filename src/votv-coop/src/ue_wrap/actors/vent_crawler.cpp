// ue_wrap/actors/vent_crawler.cpp -- see ue_wrap/actors/vent_crawler.h.

#include "ue_wrap/actors/vent_crawler.h"

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <cstdint>
#include <string>

namespace ue_wrap::vent_crawler {
namespace {

namespace R = reflection;
namespace P = profile;

// The variable's offset on the crawler's class, looked up once per class object. -1 when the class has no
// ObjectProperty of that name.
void*   g_cls = nullptr;
int32_t g_off = -1;

int32_t VentOffset(void* cls) {
    if (cls == g_cls) return g_off;
    g_cls = cls;
    g_off = -1;
    int32_t off = -1;
    std::wstring type;
    if (!R::PropertyOffsetAndClass(cls, P::name::VentCrawlerVentProp, off, type) || off < 0) {
        UE_LOGW("vent_crawler: '%ls' not found on %ls -- the crawler's vent cannot be followed",
                P::name::VentCrawlerVentProp, R::ClassNameOf(cls).c_str());
    } else if (type != L"ObjectProperty") {
        UE_LOGW("vent_crawler: '%ls' on %ls is a %ls, not an ObjectProperty -- left alone",
                P::name::VentCrawlerVentProp, R::ClassNameOf(cls).c_str(), type.c_str());
    } else {
        g_off = off;
    }
    return g_off;
}

}  // namespace

void* VentOf(void* crawler) {
    UE_ASSERT_GAME_THREAD("vent_crawler::VentOf");
    if (!crawler || !R::IsLive(crawler)) return nullptr;
    const int32_t off = VentOffset(R::ClassOf(crawler));
    if (off < 0) return nullptr;
    void* vent = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(crawler) + off);
    if (!vent || !R::IsLive(vent)) return nullptr;
    return R::ClassNameOf(vent) == P::name::PropVentClass ? vent : nullptr;
}

}  // namespace ue_wrap::vent_crawler

// coop/save/save_player_filter.cpp -- see coop/save/save_player_filter.h.

#include "coop/save/save_player_filter.h"

#include "coop/player/players_registry.h"
#include "ue_wrap/actors/inventory_component.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>

// mainPlayer_C's int_save answers. ignoreSave is a constant false in 0.10.0's bytecode; the save walk
// asks it of every int_save actor before it asks betterGetData.
UE_FACT_FN(kRowIgnoreSave, "save_player_filter", L"mainPlayer_C", L"ignoreSave", Declared);
UE_FACT_FN(kRowGetData, "save_player_filter", L"mainPlayer_C", L"betterGetData", Declared);

namespace coop::save_player_filter {

namespace R = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
namespace N = ue_wrap::profile::name;
namespace IC = ue_wrap::inventory_component;

namespace {

constexpr int kTagIgnoreSave = 0x53504946;  // 'SPIF'
constexpr int kTagGetData = 0x53504947;     // 'SPIG'

bool g_watched = false;
bool g_previewHeld = false;
Stats g_stats;

// A mainPlayer_C other than the one this process plays: a registered puppet, or any body standing
// beside a local player that is known. With no local player known nothing is excluded -- keeping a
// record is the safe error, and a duplicate is refused at the load.
bool IsForeignBody(void* actor) {
    auto& reg = coop::players::Registry::Get();
    if (reg.IsPuppet(actor)) return true;
    void* const local = reg.Local();
    return local && actor != local;
}

SG::Verdict OnIgnoreSavePre(const SG::Call& c) {
    if (!c.object || !IsForeignBody(c.object)) return SG::Verdict::Run;
    static ue_wrap::reflection::ParamOffset sIgnore{N::PropIgnoreSaveOutParam};
    uint8_t* const out = [&]() -> uint8_t* {
        const int32_t off = sIgnore.Of(c.function);
        return off >= 0 ? SG::OutParamPtr(c, off) : nullptr;
    }();
    if (!out) {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGE("save_player_filter: mainPlayer_C::ignoreSave has no '%ls' out parameter in its frame -- a "
                    "puppet or the preview mannequin would be saved as a second player record",
                    N::PropIgnoreSaveOutParam);
        }
        return SG::Verdict::Run;
    }
    *out = 1;
    if (++g_stats.excluded <= 3)
        UE_LOGI("save_player_filter: mainPlayer_C %p is not the local player -- kept out of the world save "
                "(exclusion #%u)", c.object, g_stats.excluded);
    return SG::Verdict::Cancel;
}

// The capture of a player's record: what its component holds as betterGetData serializes it. A body
// that reaches here and is not the local player escaped the exclusion above, and is said so.
void OnGetDataPost(const SG::Call& c) {
    const IC::Counts n = IC::CountItems(c.object);
    if (IsForeignBody(c.object)) {
        UE_LOGW("save_player_filter: betterGetData ran on %p, which is not the local player -- a second player "
                "record is entering the save (carried=%d hands=%d equipment=%d)", c.object, n.carried, n.hands,
                n.equipment);
        return;
    }
    ++g_stats.captured;
    UE_LOGI("save_player_filter: save captured the local player %p (component %p): carried=%d hands=%d equipment=%d "
            "slotC=%d slotD=%d", c.object, IC::Of(c.object), n.carried, n.hands, n.equipment, n.slotC, n.slotD);
}

}  // namespace

void Install() {
    if (g_watched) return;
    g_watched = true;
    bool ok = SG::WatchName(kRowIgnoreSave, kTagIgnoreSave, &OnIgnoreSavePre, nullptr);
    ok = SG::WatchName(kRowGetData, kTagGetData, nullptr, &OnGetDataPost) && ok;
    SG::ResolvePendingNames();
    if (!ok)
        UE_LOGE("save_player_filter: the script gate refused a mainPlayer_C save watch -- a puppet may be written "
                "into the world save");
}

void Pump() {
    if (!g_watched || SG::PendingNameCount() == 0) return;
    SG::ResolvePendingNames();
}

void HoldForPreview(bool on) {
    if (on == g_previewHeld) return;
    g_previewHeld = on;
    if (on) {
        Install();
        SG::Acquire("save_player_filter");
    } else {
        SG::Release("save_player_filter");
    }
}

Stats Counters() { return g_stats; }

}  // namespace coop::save_player_filter

// ue_wrap/engine/world_save.cpp -- see ue_wrap/engine/world_save.h.

#include "ue_wrap/engine/world_save.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/world/world_singleton.h"  // the gamemode, of the CURRENT world

#include <atomic>
#include <cstdint>
#include <vector>

namespace ue_wrap::world_save {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

bool SaveNow() {
    UE_ASSERT_GAME_THREAD("world_save::SaveNow");
    // This world's gamemode only: a dying world's gamemode would serialise a world that is gone
    // (ue_wrap/engine/save_capture.cpp says how that once looked).
    void* gm = world_singleton::Gamemode();
    if (!gm) {
        UE_LOGW("world_save: no mainGamemode of the current world -- nothing saved");
        return false;
    }
    void* saveSlot = *reinterpret_cast<void* const*>(
        reinterpret_cast<const uint8_t*>(gm) + P::off::AmainGamemode_saveSlot);
    if (!saveSlot) {
        UE_LOGW("world_save: mainGamemode.saveSlot is null -- nothing saved");
        return false;
    }
    void* fn = R::FindFunction(R::ClassOf(saveSlot), L"saveToSlot");
    if (!fn) {
        UE_LOGW("world_save: saveSlot_C::saveToSlot does not resolve -- nothing saved");
        return false;
    }
    // A zeroed frame: a full, non-quick, non-forced save (the game's own autosave shape).
    const int32_t frameSize = R::FunctionFrameSize(fn);
    std::vector<uint8_t> frame(frameSize > 0 ? static_cast<size_t>(frameSize) : 0u, 0u);
    if (!R::CallFunction(saveSlot, fn, frame.empty() ? nullptr : frame.data())) {
        UE_LOGW("world_save: saveToSlot did not dispatch -- nothing saved");
        return false;
    }
    UE_LOGI("world_save: saveToSlot ran (the game's own full save)");
    return true;
}

}  // namespace ue_wrap::world_save

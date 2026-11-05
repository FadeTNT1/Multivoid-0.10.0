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
    // The gamemode's save member by the name the running build gives it (save_slot on 0.10.0), never
    // a fixed offset: the old one read some other member on 0.10.0 and asked ITS class for saveToSlot.
    void* saveSlot = world_singleton::SaveSlot();
    if (!saveSlot) {
        UE_LOGW("world_save: the gamemode's save object is unresolved or null -- nothing saved");
        return false;
    }
    // saveSlot_C::save, the whole cycle the game's own pause-menu save runs: the world gathered into
    // the save object (mainGamemode.saveObjects), then saveToSlot writing it into the loaded world's
    // folder. saveToSlot alone wrote whatever the last gather left.
    void* fn = R::FindFunction(R::ClassOf(saveSlot), P::name::SaveSlotSaveFn);
    if (!fn) {
        UE_LOGW("world_save: saveSlot_C::%ls does not resolve -- nothing saved", P::name::SaveSlotSaveFn);
        return false;
    }
    // A zeroed frame: asBackup, asAutosave and skipEventBlock false, the pause menu's own call.
    const int32_t frameSize = R::FunctionFrameSize(fn);
    std::vector<uint8_t> frame(frameSize > 0 ? static_cast<size_t>(frameSize) : 0u, 0u);
    if (!R::CallFunction(saveSlot, fn, frame.empty() ? nullptr : frame.data())) {
        UE_LOGW("world_save: save did not dispatch -- nothing saved");
        return false;
    }
    UE_LOGI("world_save: saveSlot_C::save ran (the game's own full save)");
    return true;
}

}  // namespace ue_wrap::world_save

// ue_wrap/engine/world_save.h -- the game's own full world save, run on request.
//
// Engine-wrapper layer (principle 7): one UFunction call, saveSlot_C::save on the running world's
// save object (mainGamemode.save_slot) with every flag false -- the gather (saveObjects) and the
// write (saveToSlot) the game's pause-menu save runs, into the loaded world's folder. Whatever the
// mod gates on that path (coop/save/save_block) sees it as it sees the game's own. No gameplay,
// network or coop state. The dedicated server's autosave and its `save` command are the callers.
// Implementation: src/ue_wrap/engine/world_save.cpp.

#pragma once

namespace ue_wrap::world_save {

// Run the save now. False when there is no gamemode of the CURRENT world (a menu, a world change),
// its saveSlot is null, or saveToSlot does not resolve or dispatch. A client's save is cancelled by
// coop/save/save_block as any of its saves is; only a host or a solo world writes. GAME THREAD ONLY.
bool SaveNow();

}  // namespace ue_wrap::world_save

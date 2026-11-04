// ue_wrap/engine/world_save.h -- the game's own full world save, run on request.
//
// Engine-wrapper layer (principle 7): one UFunction call, saveSlot_C::saveToSlot on the running
// world's save object (mainGamemode.saveSlot), with a zeroed frame -- quicksave=false,
// overwriteSubsave=false, isForcedSave=false -- the same body the game's own save and autosave run.
// Whatever the mod hooks on that path (the disk hook of ue_wrap/engine/save_to_slot_hook, the gather
// watch of ue_wrap/engine/save_capture) sees it as it sees the game's own. No gameplay, network or
// coop state. The dedicated server's autosave and its `save` command are the callers.
// Implementation: src/ue_wrap/engine/world_save.cpp.

#pragma once

namespace ue_wrap::world_save {

// Run the save now. False when there is no gamemode of the CURRENT world (a menu, a world change),
// its saveSlot is null, or saveToSlot does not resolve or dispatch. A client's save is cancelled by
// coop/save/save_block as any of its saves is; only a host or a solo world writes. GAME THREAD ONLY.
bool SaveNow();

}  // namespace ue_wrap::world_save

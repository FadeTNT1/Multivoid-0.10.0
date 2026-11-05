// ue_wrap/engine/save_to_slot_hook.h -- the detour on UGameplayStatics::SaveGameToSlot, the
// engine's flat-slot save. On 0.10.0 the game writes its worlds through the CompressedSaveLoadLib
// plugin instead (ue_wrap/engine/world_folder), and the world-save verbs above it are gated at
// their bodies (coop/save/save_block); a few blueprints still write the live world-save container
// to a flat slot through this function, and the join capture writes its scratch slot here. The
// GATE is asked before a world save (a USaveGame that is a saveSlot_C) is written and may cancel
// it; the meta save (save_main_C: keybinds, achievements, store) passes straight through.
// Engine-wrapper layer (principle 7): no session, no role, no coop state. The gate runs on the
// thread that called SaveGameToSlot, which for every save the game makes is the game thread.

#pragma once

namespace ue_wrap::save_to_slot_hook {

// A world save is about to be written. True lets the engine write it; false cancels it, and the
// engine call returns false -- "the save did not happen", which is what the Blueprint save flow
// shows the player. `slotName` is valid for the call only. A gate that FAULTS cancels the save:
// a process that cannot decide does not write.
using Gate = bool (*)(void* saveObject, const wchar_t* slotName);

// Install the detour. Idempotent, and cheap once it has answered for good. False while
// saveSlot_C is not loaded yet -- without the class every save would read as "not a world save"
// and slip past the gate, so the detour is not armed before it -- call again later. A signature
// miss is logged once and is final.
bool Install();

// One subscriber; null clears. Any thread.
void SetGate(Gate gate);

}  // namespace ue_wrap::save_to_slot_hook

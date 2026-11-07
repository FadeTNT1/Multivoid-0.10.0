// ue_wrap/engine/engine_save_internal.h -- INTERNAL, src-tree only: shared by engine_save.cpp and
// engine_save_return.cpp; nothing else includes it. The public verbs stay in
// include/ue_wrap/engine/engine_save.h.

#pragma once

namespace ue_wrap::engine::detail {

// LoadStorySave's step (c): the slot's save registered on the game instance (loaded from disk once
// per campaign, the save-object-ready hook fired then), its rules, day, apply flag, mode and launch
// level put in force, and the open of the gameplay level (re)issued. `curWorld` is the world the
// campaign polls in. True once the open was issued; false (no travel this poll) when something is
// not up yet, or when the slot is missing or empty, which also sets `*slotMissing` when given. `who`
// names the caller on every line. Defined in engine_save.cpp. Game thread.
bool RegisterStorySaveAndOpen(const wchar_t* slot, int forceGameMode, void* curWorld, const char* who,
                              bool* slotMissing = nullptr);

// The boot survey's first answer: a live mainPlayer_C of the current world stands placed off the
// origin. Defined in engine_save.cpp. Game thread.
bool PawnPlacedInCurrentWorld(const char* who);

// The pawn stands in the level: true once the save's restore ran there (or cannot be seen, or its
// wait's cap ran out). Defined in engine_save.cpp. Game thread.
bool RestoreSettledFor(const char* who);

// The pocket return's campaign forgotten; ResetCachedSave calls it. Defined in
// engine_save_return.cpp. Game thread.
void ResetReturnCampaign();

}  // namespace ue_wrap::engine::detail

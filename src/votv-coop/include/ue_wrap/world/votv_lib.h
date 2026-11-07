// ue_wrap/world/votv_lib.h -- thunks into VOTV's shared BP function library (lib_C).
//
// Engine-wrapper layer (principle 7): resolves the lib_C CDO + UFunctions once
// and exposes typed calls. No coop/network state.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::script_gate { struct Call; }

namespace ue_wrap::votv_lib {

// Dispatch lib_C::step(Character, ...) -- THE shared footstep verb every VOTV
// walker funnels into (the local player's tick accumulator at mainPlayer uber
// @70973 and kerfurOmega.step both call it). It sphere-traces to the ground,
// picks the surface-material step cue (foot_def/grass/metal/snow/water/...),
// SpawnSoundAttached's it with att_default + conc_footsteps, and fires the
// steppedOn world reactions -- fully spatialized, possession-agnostic, nothing
// for us to re-implement. Silent on its own when airborne (no ground hit).
// Args mirror the local player's call (Z_offset=0, pitch 1.0, speedVolume=400)
// but for two. `volume` is the caller's: the BP multiplies it onto its internal
// clamp(MaxWalkSpeed/speedVolume, 0.5, 2.0) loudness, and the puppet stride
// emitter passes its own tuning. `callActor` is the character, where the local
// player's call passes null; lib_C tests it for int_objects and calls its
// `stepped`, which mainPlayer_C implements as an empty event. Returns false
// until lib_C resolves. Game thread (ProcessEvent).
bool CharacterStep(void* character, float volume);

// Dispatch lib_C::addGloss(name, level, worldContext): the signal glossary of the machine's own profile gains
// `name` at `level`, or raises it (lib.cpp:4143-4175, its save_main through getMainSave). False until lib_C
// resolves, or when the name does not convert. Game thread (ProcessEvent).
bool AddGloss(const std::wstring& name, int32_t level, void* worldContext);

// Dispatch lib_C::progressAdvancement(name, disablePopup, worldContext): in game mode 0 only, the world's save slot
// progresses the advancement by one (saveSlot_C::prAdvanc, which on reaching its target marks it completed and pays
// its award) and the laptop's list is refreshed; in any other mode it does nothing. False until lib_C resolves, or
// when the name does not convert. Game thread (ProcessEvent).
bool ProgressAdvancement(const std::wstring& name, bool disablePopup, void* worldContext);

// lib_C::addHint(inText, type, debug, __WorldContext), the one function every in-game hint (the info-icon toast,
// bottom right) is raised through: it creates a ui_hint_C, sets its `type` (enum_notifytype, the icon) and adds it
// to the game mode's hint box, unless `debug` is set outside the editor or the settings disable pop-ups. Its
// duration is the widget's own. The frame of one call, read at its entry.
struct HintCall {
    std::wstring text;           // the hint's display string (Conv_TextToString of inText)
    uint8_t      type = 0;       // enum_notifytype
    bool         debug = false;  // an editor-only hint: the game shows nothing outside the editor
    void*        worldContext = nullptr;  // the raiser's Self, as every caller passes it
};

// Read lib_C::addHint's parameters out of a script-gate call to it. False when the function carries no such
// parameter (another build) or the text does not read. Game thread (the text reads through ProcessEvent).
bool ReadAddHintCall(const ue_wrap::script_gate::Call& call, HintCall& out);

// Dispatch lib_C::addHint(text, type, false, worldContext): this machine's own hint, as the game raises it --
// same widget, icon and duration. False until lib_C resolves, or when the text does not mint. Game thread.
bool AddHint(const std::wstring& text, uint8_t type, void* worldContext);

// Whether the Blueprint chain a script-gate call was entered through began in the local player's class
// (mainPlayer_C): its input events and the verbs its use key dispatches. A body the player's own hand reached,
// as opposed to an actor's tick, timer or latent resume, or a reflected call of ours. Game thread, in a PRE.
bool BeganAtPlayer(const ue_wrap::script_gate::Call& call);


}  // namespace ue_wrap::votv_lib

// ue_wrap/desk/comp_pane.h -- the main desk's refiner (comp) pane surface: the
// decode scalars + the comp_data_0 struct base + the pane repaint / direct
// paints / cue actions -- one desk sub-surface per file, so the desk actor
// and the atlas widget chain stay owned
// by ue_wrap::console_desk (Instance() / AtlasWidget() publics). Principle-7
// engine-wrapper layer -- NO network logic; coop::comp_sync drives the
// mirror through here.
//
// The decode ticker is gated on isDreaming, active_comp and comp_isDecodeActive,
// and on nothing else -- no occupancy condition -- so any awake
// machine with the flag latched SIMULATES (and completion fires world
// triggers incl. the level-3 theEvil_C spawn). Mirrors therefore stay
// passive: raw scalar writes + direct paints + cue edges; the flag is never
// written true from the wire.

#pragma once

#include "ue_wrap/core/script_gate.h"

#include <string>

namespace ue_wrap::comp_pane {

// The names a watch on the refiner knows its functions by. A class-scoped watch is known by the pointers it was
// registered with, so a watcher registers and asks with the same ones.
inline constexpr const wchar_t* kDeskClass = L"analogDScreenTest_C";
inline constexpr const wchar_t* kCompStart = L"comp_start";          // the decode's latch: press, continue, restore
inline constexpr const wchar_t* kCalculateComp = L"calculate_comp";  // the desk's per-tick decode step and completion

struct CompScalars {
    float progress = 0;          // comp_progress      (0..100)
    float downloading = 0;       // comp_downloading   (per-tick inc; the B\s readout)
    bool  decodeActive = false;  // comp_isDecodeActive (read side)
};
bool ReadCompScalars(CompScalars& out);
// The same, off the desk in hand: a watch on the desk's own body reads the desk that body runs on.
bool ReadCompScalars(void* desk, CompScalars& out);

// Mirror-side write: progress + downloading ONLY (never the flag).
bool WriteCompScalars(float progress, float downloading);

// The live comp_data_0 struct base (signal_dynamic I/O target). Null when
// unresolved / no world. The second is off the desk in hand.
void* CompDataPtr();
void* CompDataPtr(void* desk);

// Clears a latched decode as comp_stop does: comp_isDecodeActive false, the wind-down cue, "idle". For a client's
// latch from before its session. No-op if not latched.
bool UnlatchDecode();

// comp_start's `succ` out parameter lives in the caller's storage: a start refused at the gate writes it false
// there, as comp_start's own refusals do.
bool WriteStartFailed(const script_gate::Call& c);

// The progress a comp_start begins from: 0 for a press and the completion's continue, the saved progress for
// setData's restore. False when the parameter is not found.
bool ReadStartFrom(const script_gate::Call& c, float& out);

// updComp(bool hasData): the comp pane repaint. Condition semantics are
// "has data" (comp_data_0.size > 0), which is what the native callers mean by
// it -- not whether the pane is active.
bool UpdComp(bool hasData);

// A reflected comp_start(from, succ) on the desk: its native refusals and its latch, as a press's start runs it, but
// with no press or drive around it. False when it did not dispatch; `succ` its answer.
bool CallStart(float from, bool& succ);

// Direct paints for the two texts a passive mirror leaves wrong
// (text_comp_progress only repaints inside the decode-active tick chain;
// text_comp_process inside comp_start, comp_stop and the completion, and updComp paints it "idle").
bool PaintCompProgress(float progress);
bool PaintCompProcess(const wchar_t* text);

// Decode ambience on WIRE edges -- the comp_start/comp_stop cue actions
// minus the state latch: rising -> the computerWorking_Cue loop; falling ->
// the computerWorking_end wind-down; completion -> the prog/Done beep.
bool CompCueStart();
bool CompCueStop();
bool CompBeepDone(bool maxed);

// The pane's green output console (text_compconsole), fed by the desk's own comp_writehecer(a): it prepends `a` to
// comp_hecertext, keeps the first 1500 characters and repaints the console. The native decode step writes there; a
// mirror, whose decode step never runs, calls it for the same cosmetic text. False when the function or the atlas
// widget is not there (the script would read an invalid widget).
bool WriteHecer(const wchar_t* text);
// comp_hecertext as it stands. False when unresolved or no desk.
bool ReadHecerText(std::wstring& out);

}  // namespace ue_wrap::comp_pane

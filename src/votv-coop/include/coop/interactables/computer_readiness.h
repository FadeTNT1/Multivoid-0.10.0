// coop/interactables/computer_readiness.h -- why a client's use of the base computer did or did not take effect.
//
// A client at the SAT console, the main desk's buttons or the laptop asks the host, and the host's world does the
// work; what the typist sees back depends on the host's widgets being up. When they are not, a command used to run
// and show nothing, with nothing in the log to say why. Each site that can fail that way reports through here, with
// the reason it went where it did:
//   MissingActor        the host has no desk (or laptop, or button) to act on
//   MissingWidget       the actor is there, the widget chain (desk widget, atlas, terminal) is not
//   DispatchFailed      the call into the game did not run
//   NativeNoOp          the call ran and the watched state did not move
//   WaitingContinuation the call started a latent chain; the state moves when it ends
//   OutputMerged        the output reached the shared screen
//   StateChanged        the watched state moved
// The first report of each failing reason at a site is a warning, always; with [dev] computer_readiness_trace every
// report is a line. The detail the caller passes carries the actor, the widget chain, the claim holder, the busy level
// before and after and whether the state moved. Game thread. No engine access: the callers read what they report.
#pragma once

#include <cstdint>

namespace coop::computer_readiness {

enum class Site : uint8_t {
    SatLine,      // a client's command arriving at the host
    SatMerge,     // a print written to the host's canonical terminal
    SatInput,     // the holder's input line put on the canonical terminal
    SatPublish,   // the screen published to the peers
    DeskPress,    // a client's desk button press
    LaptopPower,  // the laptop's power replay
    Count
};

enum class Why : uint8_t {
    MissingActor,
    MissingWidget,
    DispatchFailed,
    NativeNoOp,
    WaitingContinuation,
    OutputMerged,
    StateChanged,
    Count
};

// [dev] computer_readiness_trace.
bool TraceOn();

// One report. A failing why (the first four) warns once per site until the site is reset; every report logs when the
// trace is on. printf formatting, bounded.
void Report(Site site, Why why, const char* fmt, ...);

// A session's end: the site's once-per-reason latches clear.
void ResetSite(Site site);

}  // namespace coop::computer_readiness

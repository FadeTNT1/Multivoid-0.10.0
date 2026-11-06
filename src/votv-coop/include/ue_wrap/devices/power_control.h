// ue_wrap/devices/power_control.h -- engine access for the base's power panel (ApowerControl_C). Principle-7
// engine-wrapper layer: no network or coop state; coop/world/power_panel drives the lane through here.
//
// The panel holds five latched breakers, read and written here as one mask in field order: bit0 press_coord,
// bit1 press_downl, bit2 press_play, bit3 press_calc, bit4 press_light. The base has one, gamemode.powerControl.
// ButtonsVisibility is the panel's own full apply, the verb every press ends in: the panel's repaint, its
// fan-out to the servers, the light groups, the base sockets and the wall cords (each run only when its breaker
// differs from the value the last fan-out cached), and gamemode.setPower while the panel is not disabled and the
// generators are fine. Verbs resolve on the panel in hand, since a Blueprint class can be a new object in a new
// world.

#pragma once

#include <cstdint>
#include <vector>

namespace ue_wrap::power_control {

// Resolve powerControl_C's five breakers, disabled, waterlogged and the apply verb, all by name; a miss on a loaded
// class refuses the wrapper for the process (no write at a stale offset). Idempotent; a class not loaded yet is
// asked again a second later. Game thread.
bool EnsureResolved();

// True iff `obj`'s class is powerControl_C or a subclass. False if not yet resolved.
bool IsPowerControl(void* obj);

// The base's panel, gamemode.powerControl, or null before the game mode holds it. Game thread.
void* Panel();

// The five breakers as the mask above. False on null or unresolved; `mask` untouched then.
bool ReadPress(void* p, uint8_t& mask);

// Write the five breakers from the mask, raw: nothing repaints or powers until ButtonsVisibility. Game thread.
bool WritePress(void* p, uint8_t mask);

// The panel's `disabled` (the desk virus's 60 s lockout, virus_pb) and `waterlogged` (the basement drain's flood
// flag, which the laptop's breaker page refuses on). Game thread.
bool ReadDisabled(void* p, bool& disabled);
bool WriteDisabled(void* p, bool disabled);
bool ReadWaterlogged(void* p, bool& waterlogged);
bool WriteWaterlogged(void* p, bool waterlogged);

// The lever component of breaker `bit` (the mask's order), which a player's trace takes and E presses, or null.
// Game thread.
void* Lever(void* p, int bit);

// Press breaker `bit`'s lever as the player's E dispatch does: actionOptionIndex(player, a hit on the lever, the
// lever), whose own route gates on the tutorial, flips the breaker, clicks and applies. Game thread.
bool PressLever(void* p, void* player, int bit);

// Run the panel's buttonsVisibility() as the game runs it. Game thread.
bool ButtonsVisibility(void* p);

// Play the lever's click as a press plays it, playSND(on): heard only while the panel is powered. Game thread.
bool PlayLeverSound(void* p, bool on);

// Switch the panel's servers and their ambient hum, when it has one, on or off: each server's setActive(on) and the
// hum's SetActive(on, false), as the desk virus's lockout (virus_pb) does at its start and, with the calc breaker,
// at its end. Game thread.
bool SetServersActive(void* p, bool on);

// The lockout's end cue: stationTurnon, 2D, as the panel's own end plays it. Game thread.
bool PlayTurnOnCue(void* p);

// The panel's light groups and the doors it opens in a blackout (its lighRoots and doorsOpen): what its
// solar() turns off and opens. The live ones, in the panel's order; false when the member does not
// resolve (said once). Game thread.
bool ReadLightRoots(void* p, std::vector<void*>& out);
bool ReadBlackoutDoors(void* p, std::vector<void*>& out);

// The units' power as the panel's setPower left it in the game mode: its five usesp_ flags and powerUsage.
struct UnitPower {
    bool  calc = false, downl = false, coords = false, play = false, light = false;
    float usage = 0.f;
};

// How many of the panel's servers run their loop, and its hum: -1 when the panel has none, else whether it plays.
struct ServerState {
    int active = 0, total = 0;
    int hum = -1;
};

// [dev] the grid drill's readings: the units' power, and the panel's servers. Game thread.
bool ReadUnitPower(UnitPower& out);
bool ReadServers(void* p, ServerState& out);

// [dev] the grid drill's lockout: the panel's virus_pb(), as the desk's virus runs it. Game thread.
bool CallVirusLockout(void* p);

// [dev] the keypad drill's stale arm: the game mode's usesp_light written raw, as a client was left before its
// panel ran setPower from the host's canonical. Game thread.
bool WriteLightPower(bool on);

}  // namespace ue_wrap::power_control

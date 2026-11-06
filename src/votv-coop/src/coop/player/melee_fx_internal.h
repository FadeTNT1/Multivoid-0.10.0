// coop/player/melee_fx_internal.h -- what coop/player/melee_fx*.cpp share: one effect as a maker describes it, and the
// publish that packs, paces and sends it.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::melee_fx::detail {

// One effect, before it is on the wire. `sound` and `particle` are asset names as ue_wrap::engine::SoundName writes
// them ("SoundCue /Game/audio/x.x"); empty is no such effect.
struct Effect {
    uint8_t         kind = 0;        // coop::net::kMeleeFx*
    uint32_t        sourceEid = 0;   // the held prop, for a contact or break
    uint32_t        targetEid = 0;   // the struck element, when the maker knew it
    ue_wrap::FVector loc{};
    ue_wrap::FVector normal{};
    float           scale = 1.f;
    float           volume = 1.f;
    float           pitch = 1.f;
    std::wstring    sound;
    std::wstring    particle;
};

// The kill switch (the `melee_fx` row), asked once per process.
bool Enabled();

// The `melee_fx_log` row: every line instead of the first few of each kind.
bool LogAll();

// The session while it is connected, else null.
coop::net::Session* LiveSession();

// Pack `e` and send it: a client's as MeleeFxRequest to the host, the host's as MeleeFx to every client. False when
// nothing was sent: switched off, no session, over the maker's pace, an asset name the wire cannot carry (said once),
// or the send failed. Game thread.
bool Publish(const Effect& e);

// Play the particle of `e` here, for this machine's own player, as it makes the effect. For a surface effect no native
// call of the swing shows its maker (a material with no impact particle, whose damaged one the hit stands in for):
// Publish only sends it to the others. Log line "local play own". Call before Publish of the same effect. Game thread.
void PlayOwn(const Effect& e);

// FNV-1a of `text`'s UTF-16 code units, low byte first: the id of a held item's key on the wire.
uint32_t KeyHash(const std::wstring& text);

// The seams' own entry points, one pair of files each: the swing (melee_fx_swing.cpp) and a held prop's contact
// (melee_fx_contact.cpp). Install takes the watches, Tick settles them and closes what a faulted body left open, Reset
// forgets a session's latches. Game thread.
void InstallSwing();
void TickSwing();
void ResetSwing();
void InstallContact();
void TickContact();
void ResetContact();

}  // namespace coop::melee_fx::detail

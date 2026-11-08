// coop/player/nameplate.h -- floating nickname labels above remote players (ImGui screen-space).
//
// Gameplay/network layer (principle 7), the MTA nametag shape drawn by our own ImGui overlay (VOTV never
// runs the stock HUD canvas). Update() (the ~60 Hz pump) decides WHAT each plate shows and its WORLD
// anchor. The frame capture decides WHERE: a post-observer on the HUD widget's per-frame UMG Tick, which
// runs after the camera update and before the present is queued, re-reads each anchor and projects it
// with that frame's camera into a numbered ring. The render thread (ui::hud) counts presents
// (NotePresent) and draws the capture paired with its own frame. Projected on the pump, a plate held one
// screen point over several frames and shook on fast turns. No HUD tick, or ui.nameplate.frame_sync off:
// Update's own projection is drawn.

#pragma once

#include "coop/net/link_kind.h"
#include "coop/player/players_registry.h"  // kMaxPeers
#include "coop/text/utf8_codec.h"          // kNickBufBytes -- the nick buffer's ONE owner

namespace coop::net { class Session; }

namespace coop::nameplate {

// One projected label (plain data; the render thread reads it). nick is a fixed UTF-8 buffer sized
// by the display policy (coop::text::kNickBufBytes) -- the same alphabet the roster board and the
// chat bubble carry, so a non-Latin name renders here as it does everywhere else.
struct Plate {
    float x = 0.f;           // screen px (viewport pixels, top-left origin) -- the head anchor
    float y = 0.f;
    float wx = 0.f;          // the WORLD anchor (cm) the screen point projects; the frame capture
    float wy = 0.f;          // re-reads it for an online peer and reuses it for an offline sleeper
    float wz = 0.f;
    int   slot = -1;         // the peer slot of an online plate; -1 for an offline sleeper
    float alpha = 0.f;       // distance fade 0..1 (0 => skip)
    float scale = 1.f;       // distance SIZE scale: 1 = base size up close, shrinks ~1/dist far away
    bool  onScreen = false;  // projected in FRONT of the camera AND within fade range
    bool  occluded = false;  // world geometry between the camera and the head -> the plate
                             // renders GRAY
    bool  flash = false;     // hurt-flash red (read from RemotePlayer::IsHurtFlashing)
    bool  sleeping = false;  // the peer rests or dreams (sleep_sync) -> the HUD floats "z Z Z" off the plate
    // A sleeper (coop/player/sleepers), an offline player lying in the world: "z Z Z", no ping and no health bar;
    // the name only within the sleeper name distance, and the loot hint while the viewer can loot it. A sleeper
    // behind a wall has no plate at all.
    bool  sleeper = false;
    bool  showNick = true;
    bool  lootHint = false;
    bool  joining = false;   // a sleeper that is a connecting player's joining body: "(joining...)" under the name
    // An online sleeper the viewer may push awake (coop/player/push_wake): 0 none, 1 "[RMB x2] Wake", 2 the cooldown
    // the host named, `wakeSecs` of it left. Drawn where the loot hint sits.
    uint8_t wakeHint = 0;
    int   wakeSecs = 0;
    bool  tipsy = false;     // the peer drank wine or beer lately (coop/player/tipsy): bubbles beside the plate
    int   healthPct = 100;   // 0..100 streamed-vitals health (display-only)
    int   ping = -1;         // RTT to the SESSION in ms (0 = sub-ms LAN -> "<1ms")
    // Carried so the plate does not have to INFER "no number to show" from ping == -1. The host has
    // no link to the session, and a plate has only suffix or no suffix -- no room for the
    // scoreboard's "n/a". Passing the kind makes that structural instead of an agreement between
    // two files.
    coop::net::LinkKind linkKind = coop::net::LinkKind::Unknown;
    uint8_t voiceIcon = 0;   // coop::voice_chat::VoiceIcon badge right of the plate (0 = none)
    uint32_t colorRGB = 0;   // packed custom nick color (coop::nick_color; 0 = white)
    float bubbleAlpha = 0.f; // overhead chat bubble fade (0 = none; rides the plate anchor)
    char  bubble[208] = {};  // the peer's last chat message, UTF-8 (chat_bubbles)
    char  nick[coop::text::kNickBufBytes] = {};
};

// Sleepers drawn at most at once, the nearest first-come (the overlay is a budget, not a census).
inline constexpr int kMaxSleeperPlates = 16;

struct Snapshot {
    int   count = 0;
    Plate plates[coop::players::kMaxPeers + kMaxSleeperPlates];
};

// GAME THREAD: project every live remote puppet -> a fresh snapshot, then publish.
// Cheap no-op when there are no puppets (early-out before the controller resolve) /
// no local player. Called UNCONDITIONALLY from the harness tick (~60 Hz) so the
// snapshot self-clears to empty at the menu (the HUD then auto-hides).
void Update();

// Copy the snapshot to draw this frame: the frame capture paired with the present being built, or
// Update's own projection when there is no live capture. Render thread (it reads the present pairing
// NotePresent set up); safe from any thread.
void GetSnapshot(Snapshot& out);

// RENDER THREAD, once per overlay frame (each present), before any GetSnapshot of that frame: advances
// the present count the frame captures are paired against. Cheap; called whether or not a plate draws.
void NotePresent();

// True if there is at least one label to draw -- the overlay uses this (lock-free)
// to decide whether to run the always-on HUD pass. Any thread.
bool HasAny();

// ---- per-player visibility pref: any peer can hide its OWN plate, synced so every peer, late
// joiners included, agrees ----
// The VISIBILITY AXIS is owned HERE, in the nameplate domain; the wire layer (player_handshake)
// parses Join, PlayerJoined and NameplateChange and stores into this module, exactly as skins store
// into RemotePlayer::ApplySkin. The whole store is ATOMIC and any-thread: writers span the game
// thread (the wire handlers and the `nameplate` row's subscriber) and the bringup thread (the
// session-start reset through player_handshake::Reset on the TimelineThread); the render thread
// (the F1 checkbox) only reads.

// Boot-time init from multivoid.ini nameplate= (harness, before the pump ticks).
void SetInitialLocalVisible(bool visible);

// The local pref. Any thread (atomic) -- the F1 checkbox reads it.
bool LocalVisible();

// Follow the `nameplate` row: once, at boot.
void SubscribeRow();

// Wire store: peer `slot` announced its pref. Update() skips hidden slots from
// the next snapshot on. Any thread (atomic slot flags).
void StoreVisibleForSlot(int slot, bool visible);
bool VisibleForSlot(int slot);

// Session wiring and lifecycle edges. subsystems installs; ResetSlots runs at
// session start (bringup thread) and again at the leave-world funnel; and the
// roster ledger's occupant-change edge calls OnSlotDisconnected, which resets the
// slot to VISIBLE so a reused slot never inherits the departed peer's pref.
void Install(coop::net::Session* session);
void ResetSlots();
void OnSlotDisconnected(int slot);

}  // namespace coop::nameplate

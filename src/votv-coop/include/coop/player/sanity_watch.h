// coop/player/sanity_watch.h -- the sanity instrument: what the game's per-player sanity is doing on
// this machine, and what drives it, in the log of every peer. Read-only; no wire.
//
// Sanity is the game's own and stays per player (never shared, never written by the mod): each
// machine's ticker_lightSanity_C samples the light at its own player's hand. Its one recovery is
// light at or above the floor, so the questions a paranoid client raises are answered by its light
// sample, the floor, the drain, the binding of the sampled body, the clock its sky derives from and
// the lights in reach. The same line on the host is the baseline a client's is compared with:
//   sanity_watch[<role>]: sanity=.. tier=.. light=.. (hand ..+..) lerp=.. floor=../.. dayMult=..
//       drain=.. -> recovering|draining|paused (asleep)|off | rule=.. manual=.. save=.. shadows=..
//       flashlight=.. | clock hh:mm (day=.. / maxTime=..) day number .. [host day number ..] realtime=..
//       | at (x, y, z)
//   sanity_watch[<role>]: tier a -> b (...)           at once, on every tier change
//   sanity_watch[<role>]: lights near the hand: N walked, M in reach, K lit -- nearest: ...
//       every 30 s while draining, every 120 s otherwise
//   sanity_watch[<role>]: the ticker samples <body>, not this machine's player <body>   WARN, once per change
// What a client's rejoin starts from: the first read after a world load is said with its value (the
// game saves no sanity, so every load starts at the class default).
#pragma once

namespace coop::net { class Session; }

namespace coop::sanity_watch {

// Keep the session (for the role and the connection). Game thread.
void Install(coop::net::Session* session);

// Per pump tick: the periodic lines while connected. Cheap between lines. Game thread.
void Tick();

// Session teardown: a last line, and the state reset for the next world. Game thread.
void OnDisconnect();

}  // namespace coop::sanity_watch

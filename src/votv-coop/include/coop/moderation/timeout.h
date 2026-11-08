// coop/moderation/timeout.h -- the host's table of players held in a timeout, and the orders that
// keep their machines honest.
//
// A timeout is the host's decision about a PERSON: the table is keyed by the proved player id, not
// by the seat, so a player who drops and rejoins finds the clock still running. Each tick the host
// finds the seat that person holds and, once per seat, sends it the jail order with the seconds that
// remain (stat_orders, op Jail); the player's own machine holds its pawn (coop/player/timeout_hold)
// and ends the hold by its own clock, so a lost release cannot strand anyone. An entry is dropped at
// its end, by Release and with the session. The host itself cannot be held. Nothing persists past the
// session: the table is memory only.
//
// The verbs act only inside a running hosted session; the command handlers
// (coop/commands/timeout_commands) are the callers and the dispatcher has already decided who may
// ask. Game thread.

#pragma once

#include <string>
#include <string_view>

namespace coop::net { class Session; }

namespace coop::timeout {

// What the jail is called, in every line the mod writes about it: change it here and nowhere else.
inline constexpr char kJailName[] = "gay baby jail";

// How long a timeout lasts.
inline constexpr int kSeconds = 120;

// "2 minutes" for kSeconds (whole minutes), else "<n> seconds".
inline std::string DurationText() {
    if (kSeconds % 60 == 0) {
        const int m = kSeconds / 60;
        return std::to_string(m) + (m == 1 ? " minute" : " minutes");
    }
    return std::to_string(kSeconds) + " seconds";
}

enum class Result : unsigned char {
    Done,        // the entry stands (a restart when there was one)
    NoSession,   // no running hosted session
    NoId,        // the player's id is not proved yet
    IsHost,      // the host's own id: the host cannot be held
    NotJailed,   // Release: nobody with that id is held
};

void Install(coop::net::Session* session);

// Hold the player with this id for kSeconds from now; held already, the clock restarts. `nick` is for
// the log. The order reaches the machine on the next tick that finds the player seated and ready.
Result Issue(std::string_view playerId, std::string_view nick);

// End the timeout of the player with this id now and tell their machine.
Result Release(std::string_view playerId);

// Whether the player with this id is held by the table. Any thread.
bool IsJailed(std::string_view playerId);

// Every pump tick: drop ended entries, send the order to a seat that owes one. Host only.
void Tick();

// A seat's player left: the order delivered or sent to it is forgotten, the entry stays.
void OnSlotDisconnected(int slot);

// The session ended: the table is cleared.
void OnDisconnect();

}  // namespace coop::timeout

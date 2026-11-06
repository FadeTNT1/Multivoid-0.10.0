// coop/interactables/sat_console_sync.h -- the SAT console's screen, shared, and its shared commands, run on the host.
//
// THE SCREEN. A machine has one terminal, the ui_console every SAT console panel shows, and every peer's shows
// the same thing: the host's terminal is the canonical screen, and the others are replicas of it. The host
// owns the input line (the text the sat claim's holder is typing), the printed history (consoleLine, kept to
// its rightmost 3000 characters by the terminal's own fixLog), the prompt (its name and dish) and the busy
// flag, and publishes them whole to every ready peer as a Screen revision whenever any moved, at most ten
// times a second. The holder's input travels on its own, faster and smaller: the holder sends its input line
// to the host as it types (about twenty times a second, and at once on a submit), the host puts it on its
// terminal and relays it to the rest. Only the claim's holder may send input or output, the host checks the
// sender against its claim table, and every packet carries the holder's claim count and a sequence so one
// that is older than the last taken never overwrites newer typing. A peer whose terminal is not up yet keeps
// the newest screen and shows it when it is; a peer that joins is sent the whole screen as its world is ready.
// A replica never calls enterCommand: what it shows, it was told.
//
// THE COMMANDS. A client's line whose command rests on the shared world (coop/interactables/sat_console_table)
// is refused at its terminal's enterCommand while the terminal is not busy, its input consumed as the body's
// tail does, and sent to the host with the terminal's context: its dish or ROOT, its name, the panel last used.
// The host keeps a terminal of the game's class for each typist, by the typist's identity, never constructed or
// shown, and runs the line there, so the command happens once, in the host's world, and its effects cross by
// their own lanes. Each line that terminal prints is written to the host's own terminal, in order, which puts it
// on the shared screen; the typist's busy level goes back to the typist alone. A line whose effect is the
// typist's own (sv.target moves its compass, rd.filter its radar) runs where it was typed, and what it printed
// is sent to the host in order, so the shared history reads as it did; `clr` is the shared reset. A client's
// terminal prints nothing of its own outside a command: the world's alerts reach the host's terminal and come
// back on the screen, so a client's own would only be a second copy. A typist that leaves mid-command while
// another client keeps the session leaves the command running, and back, is bound to its terminal again as
// its world is ready; the last client's leave ends the session, and every terminal is discarded, its command
// stopping there. MTA sends a command the client does not own to the server, which runs it as that player
// (reference/mtasa-blue/Client/mods/deathmatch/ClientCommands.cpp:91,
// reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2406); MTA also runs the client's own
// handlers first (:60), which here would run the one game handler twice, so a line runs on one machine.
#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::net {
class Session;
struct BlobChunkPayload;
}

namespace coop::sat_console_sync {

void Install(coop::net::Session* s);
void Tick();
void OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
// A peer's world is ready: a typist back at a terminal still running is bound to it again, and the host sends
// the peer the whole screen. A peer's slot is gone: its chunks and owed blobs are forgotten; on the host its
// terminal is kept.
void OnPeerWorldReady(int slot);
void OnPeerGone(uint8_t slot);
void OnDisconnect();

// [dev] What the lane did, for the drill: the lines this client sent to run on the host, and on the host the
// lines it ran for clients, the lines those runs printed (written to the shared screen), the typists bound
// again to a terminal they had left, and those whose terminal was busy then; and the terminals the host
// keeps. The rest count the shared screen's traffic.
struct Counts {
    uint64_t linesSent = 0;
    uint64_t linesRun = 0;
    uint64_t linesReturned = 0;
    uint64_t rebinds = 0;
    uint64_t busyRebinds = 0;
    uint64_t screensSent = 0;     // host: screen revisions sent (one per peer)
    uint64_t screensApplied = 0;  // client: screens shown
    uint64_t screensHeld = 0;     // client: screens kept back for the typist's own unmerged output, or a terminal not up
    uint64_t inputsSent = 0;      // the holder's input packets
    uint64_t inputsTaken = 0;     // host: input packets from the holder put on the screen
    uint64_t inputsStale = 0;     // host: input packets refused as older than the last, or from a peer that is not the holder
    uint64_t presentsSent = 0;    // client: lines a command it ran itself printed, sent on
    uint64_t presentsMerged = 0;  // host: those, written to the screen
};
Counts LaneCounts();
size_t KeptTerminals(void** out, size_t cap);

}  // namespace coop::sat_console_sync

// coop/interactables/sat_console_sync.h -- the SAT console's screen, shared, and its shared commands, run on the host.
//
// THE TERMINALS. Every placed SAT panel is a terminal with a screen of its own (ue_wrap/sat_console
// TerminalBinding): ROOT at the desk, and one per field panel at its dish. The game has one ui_console per machine
// that every panel shows, and a panel's con() re-points it at that panel's context on a use AND on any player's
// begin-overlap -- so another player walking past ROOT re-pointed the console a typist at a field panel was using. In
// a session the lane refuses every con() and native init of that console, and shows on it only the terminal this
// machine's own player used, committed at or walked into (a switch waits for a command the console runs itself); a
// panel whose dish is not set is not ready, never ROOT. This is the fallback of a render per panel: the one native
// widget swaps between per-terminal states, so other panels on the same machine show the same picture meanwhile.
//
// THE SCREEN. A terminal's screen is the host's: a hidden terminal of the game's class it keeps per panel (never
// constructed or shown, so no world alert is bound to it), pointed at the panel's context, owns its printed history
// (consoleLine, kept to its rightmost 3000 characters by the terminal's own fixLog), its prompt, the holder's input
// and the busy level, and is published whole to every ready peer as a Screen revision of that terminal whenever any
// moved, at most ten times a second per terminal. The host's own console is a replica of the terminal it shows, like
// a client's. A panel is claimed per panel ("sat_<id>"), so two peers type at two terminals at once. The holder's
// input travels on its own, faster and smaller: the holder sends it to the host as it types (about twenty times a
// second, and at once on a submit), the host puts it on that terminal and relays it to the rest. Only that panel's
// claim holder may send input or output for it, its prompt and dish must be the panel's context on the host, and every
// packet carries the holder's claim count and a sequence so one older than the last taken never overwrites newer
// typing. Every blob names its terminal; a client keeps the newest screen of every terminal, bound here or not yet,
// and shows one when its console shows that terminal; a peer that joins is sent every terminal's screen as its world
// is ready. A replica never calls enterCommand: what it shows, it was told.
//
// THE COMMANDS. A client's line whose command rests on the shared world (coop/interactables/sat_console_table)
// is refused at its console's enterCommand while the console is not busy, its input consumed as the body's
// tail does, and sent to the host with the terminal it was typed at and the context shown: its dish or ROOT, its name.
// The host keeps an executor -- a terminal of the game's class, never constructed or shown -- for each typist at each
// terminal, by the typist's identity and the terminal id, and runs the line there in the host panel's own context, so
// the command happens once, in the host's world, and its effects cross by their own lanes. Each line an executor
// prints, now or from a latent continuation, is written to the canonical screen of the terminal it was made for, in
// order; the typist's busy level goes back to the typist alone, and the terminal's screen is busy while any executor
// there runs. At the cap of sixteen an idle executor gives way, so a typist can visit any number of panels.
//
// A line whose effect is the typist's own (sv.target moves its compass, rd.filter its radar) runs where it was
// typed, and what it printed is sent to the host in order, so that terminal's history reads as it did; `clr` is the
// terminal's reset. A client's console prints nothing of its own outside a command: the world's alerts reach the
// host's console and land on ROOT's screen (a panel's own alert on that panel's), so a client's own would only be a
// second copy. A typist that leaves mid-command while another client keeps the session leaves the command running,
// and back, is bound to its executor again as its world is ready; the last client's leave ends the session, and
// every executor is discarded, its command stopping there. MTA sends a command the client does not own to the server, which runs it as that player
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
// The executors the host keeps (one per typist and terminal).
size_t KeptTerminals(void** out, size_t cap);
// [dev] The terminal this machine's console shows (0: none yet), and a switch of it to the panel of a dish, as a use
// of that panel would make it. False when no ready panel carries the dish or the console is not up.
uint32_t ShownTerminal();
bool ShowTerminalForDish(int32_t dish);

}  // namespace coop::sat_console_sync

// coop/interactables/sat_console_sync.cpp -- see coop/interactables/sat_console_sync.h.

#include "coop/interactables/sat_console_sync.h"

#include "coop/config/config.h"
#include "coop/interactables/computer_readiness.h"
#include "coop/interactables/device_occupancy.h"  // LocalHolds, HolderOf: who holds a SAT panel
#include "coop/interactables/sat_console_table.h"
#include "coop/interactables/sat_console_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"     // Local: whose begin-overlap re-points this machine's console
#include "coop/session/player_handshake.h"  // GuidForSlot: a typist's durable identity

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/gc_pin.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/console_desk.h"  // Instance: the desk a host terminal names as its panel
#include "ue_wrap/desk/dish.h"          // IndexOf, DishByIndex, Count
#include "ue_wrap/desk/sat_console.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <wchar.h>

namespace coop::sat_console_sync {
namespace {

namespace SC = ue_wrap::sat_console;
namespace T  = coop::sat_console_table;
namespace V  = coop::sat_console_wire;
namespace DO = coop::device_occupancy;
namespace CR = coop::computer_readiness;
namespace GT = ue_wrap::game_thread;
namespace WI = ue_wrap::world_identity;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;
using coop::net::kMaxPeers;
using coop::net::ReliableKind;
using TermId = uint32_t;

constexpr int    kLinesPerWindow = 4;     // a person types slower: lines a typist may run per window
constexpr auto   kWindow         = std::chrono::seconds(1);
constexpr auto   kSpawnEvery     = std::chrono::seconds(2);  // a typist's spawn-on-every-call commands
constexpr size_t kMaxExecutors   = 16;    // kept executors, by (identity, terminal), while the session lasts
constexpr size_t kMaxTermStates  = 128;   // terminals with state here; far past any map's panels
constexpr size_t kOutboxCap      = 512;   // blobs owed to one slot while its sends are refused
constexpr auto   kResendEvery    = std::chrono::milliseconds(250);  // a refused slot is tried this often
constexpr auto   kScreenEvery    = std::chrono::milliseconds(100);  // a terminal's screen goes out at most ten times a second
constexpr auto   kInputEvery     = std::chrono::milliseconds(50);   // the holder's input goes out at most twenty times a second
constexpr auto   kPollEvery      = std::chrono::milliseconds(100);  // the input is read this often when no watch tells of a change
constexpr auto   kHeldGrace      = std::chrono::seconds(2);         // a screen kept back for the typist's own output waits this long
constexpr auto   kLineWaitMax    = std::chrono::seconds(10);        // a client's line kept for a terminal that is not up, then answered not ready
constexpr size_t kMaxWaitingPerSlot = 8;                            // lines one typist may have kept, over every terminal
constexpr size_t kOwedCap        = 256;                             // prints kept for one terminal while it is not up
constexpr int    kOwedAttempts   = 5;                               // a print that reached a live terminal and still failed this many times is dropped
constexpr auto   kPublishRetry   = std::chrono::milliseconds(500);  // a screen that could not be published is tried again this often
constexpr auto   kTickLinger     = std::chrono::seconds(2);         // the console keeps ticking offscreen this long after the last remote work
constexpr auto   kPanelRefresh   = std::chrono::seconds(2);         // the host re-reads the panel list this often
constexpr auto   kShowRetry      = std::chrono::seconds(1);         // the console's first terminal is tried this often until it is shown
constexpr int    kDiagSay        = 8;                               // con/init decisions said at info before the trace takes over
const wchar_t* const kBusyLine = L"<r>The terminal is busy</>";  // the game's answer to a line it cannot take
const wchar_t* const kErrLine  = L"<r>err</>";                   // the game's answer to a line it cannot run
const wchar_t* const kNotReadyLine = L"<r>The terminal is not ready</>";  // a line the host's terminal never came up for

UE_FACT_FN(kRowEnterCommand, "sat_console_sync", L"ui_console_C", L"enterCommand", Declared);
UE_FACT_FN(kRowUbergraph, "sat_console_sync", L"ui_console_C", L"ExecuteUbergraph_ui_console", Declared);
UE_FACT_FN(kRowWriteToLog, "sat_console_sync", L"ui_console_C", L"writeToLog", Declared);
// init(name, hide, dish): the prompt and dish of a terminal. Watched on this machine's console only, where every call
// in a session is ours (a native one is refused and said: it would re-point the screen at another panel's context).
UE_FACT_FN(kRowInit, "sat_console_sync", L"ui_console_C", L"init", Declared);
// The panel's con() (0.10.0 capture C:210219): init of the shared console with this panel's dish or ROOT. It runs on a
// use and on ANY player's begin-overlap, so in a session it is refused and the console shows the panel's terminal
// only when this machine's own player used it or walked into it.
UE_FACT_FN(kRowCon, "sat_console_sync", L"panel_SATconsole_C", L"con", Declared);
UE_FACT_FN(kRowOverlap, "sat_console_sync", L"panel_SATconsole_C",
           L"BndEvt__updName_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
// The input box's changed-text handler (0.10.0 bytecode capture): its body only scrolls the log on j and k and
// leaves the text alone, so it is watched for the moment the text moved, and refused while a replica shows text.
UE_FACT_FN(kRowTextChanged, "sat_console_sync", L"ui_console_C",
           L"bndevt__umg_console_EditableTextBox_K2Node_ComponentBoundEvent_2_OnEditableTextBoxChangedEvent__DelegateSignature",
           Declared);
// The alternate keyboard path: a key lands in typeSymbol, which appends to the input line `command`.
UE_FACT_FN(kRowTypeSymbol, "sat_console_sync", L"ui_console_C", L"typeSymbol", Declared);
UE_FACT_FN(kRowKeyDown, "sat_console_sync", L"ui_console_C", L"OnKeyDown_0", Declared);
constexpr int kTagEnter   = 0x53434E45;  // 'SCNE'
constexpr int kTagWrite   = 0x53434E57;  // 'SCNW'
constexpr int kTagGraph   = 0x53434E47;  // 'SCNG'
constexpr int kTagChanged = 0x53434E43;  // 'SCNC'
constexpr int kTagType    = 0x53434E54;  // 'SCNT'
constexpr int kTagKey     = 0x53434E4B;  // 'SCNK'
constexpr int kTagInit    = 0x53434E49;  // 'SCNI'
constexpr int kTagCon     = 0x53434E4F;  // 'SCNO'
constexpr int kTagOverlap = 0x53434E56;  // 'SCNV'

// Why a line, a print or a packet went where it did and not where one would expect: a bounded set of
// counters, the first of each said once, the totals said when the session ends. [sat_console_trace] says each.
enum class Reason : uint8_t {
    LocalOwnCommand,    // a client's line ran at its own terminal: the command is the typist's own
    LocalBusy,          // ... its terminal was busy, and answers the line itself
    LocalUnreadable,    // ... its terminal's flag or name could not be read
    LocalNotTerminal,   // ... it was not typed at this machine's terminal, or no terminal is shown
    AlertSuppressed,    // a client's terminal printed outside any command: the host's copy of it is the one shown
    NotHolder,          // a packet from a peer the claim table does not name as holding that terminal
    StaleInput,         // an input or present packet no newer than the last taken
    BadPacket,          // a packet that did not decode within its bounds
    WrongDirection,     // an op that goes the other way, or one retired
    NoScreenTerminal,   // the host has no canonical terminal for the panel (not bound, not ready, not made)
    ScreenHeld,         // a screen kept back, for the typist's own unmerged output or a terminal not up
    NoContext,          // the panel's context is not ready, or its init failed
    LineKept,           // a client's line waited, unrun, for its terminal to come up
    LineNotReady,       // ... and the terminal never did within the wait: the typist was told
    PrintKept,          // a print waited for its terminal to come up
    InputKept,          // the holder's input waited for its terminal to come up
    UnknownTerminal,    // a packet named a terminal no panel here carries
    ContextMismatch,    // a client's prompt or dish is not its panel's context here
    BlockedCon,         // a panel's con() was refused: another player walked into it, or this one is inside another
    BlockedInit,        // a native init of this machine's console was refused: only the lane points it now
    Count
};
constexpr size_t kReasons = static_cast<size_t>(Reason::Count);
const char* const kReasonText[kReasons] = {
    "a line ran at its typist's own terminal: the command is the typist's own",
    "a line ran at its typist's own terminal: the terminal was busy",
    "a line ran at its typist's own terminal: the terminal could not be read",
    "a line ran at its typist's own terminal: it was not typed at this machine's console, or no terminal is shown",
    "a client's terminal printed outside a command: not shown, the host's copy is",
    "a packet came from a peer that does not hold that SAT terminal: dropped",
    "an input or output packet was no newer than the last taken: dropped",
    "a packet did not decode within its bounds: dropped",
    "an op arrived the wrong way round: dropped",
    "the host has no canonical terminal for the panel yet (not bound, not ready, or not made)",
    "a screen was kept back (the typist's own output is not merged yet, or no terminal is up)",
    "the panel's context is not ready, or its init failed: the prompt stays",
    "a client's line was kept, not run, until its terminal came up",
    "a client's line waited ten seconds for its terminal and was answered not ready, never run",
    "a print was kept until its terminal came up, and written in order then",
    "the holder's input was kept until its terminal came up",
    "a packet named a terminal no panel here carries: dropped",
    "a client's prompt or dish did not match its panel's context here: refused",
    "a panel's con() was refused: it would have re-pointed this machine's console at a terminal its player is not at",
    "a native init of this machine's console was refused: only the lane points it at a terminal in a session",
};
uint64_t g_reason[kReasons] = {};
bool     g_reasonSaid[kReasons] = {};

bool Trace() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::sat_console_trace);
    return s;
}

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false;
coop::blob_chunks::Assembler g_asm;
uint32_t g_seq = 1;
Clock::time_point g_nextSweep{};
Counts g_counts{};
bool g_saidOffThread = false;
void* g_world = nullptr;  // the world the screen state below belongs to
bool g_applying = false;  // our own writes to a terminal are being made: its handlers are not a person's typing
bool g_saidPoll = false;

struct Applying {
    bool prev;
    Applying() : prev(g_applying) { g_applying = true; }
    ~Applying() { g_applying = prev; }
    Applying(const Applying&) = delete;
    Applying& operator=(const Applying&) = delete;
};

// A client's text, as the host's log shows it: printable characters only, so a line cannot forge one.
std::wstring Printable(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t ch : s) out.push_back(ch < 0x20 || ch == 0x7F ? L'?' : ch);
    return out;
}

void Note(Reason r, const std::wstring& what = std::wstring()) {
    const size_t i = static_cast<size_t>(r);
    ++g_reason[i];
    if (!g_reasonSaid[i]) {
        g_reasonSaid[i] = true;
        UE_LOGI("sat_console: %s%s%ls%s (first; the totals come when the session ends)", kReasonText[i],
                what.empty() ? "" : " [", Printable(what).c_str(), what.empty() ? "" : "]");
    } else if (Trace()) {
        UE_LOGI("sat_console: %s%s%ls%s", kReasonText[i], what.empty() ? "" : " [", Printable(what).c_str(),
                what.empty() ? "" : "]");
    }
}

// Blobs owed to a slot whose send was refused. A blob goes whole or not at all, and one refused waits
// at the head, so a slot's blobs arrive in the order they were made. The host keeps one per client's
// slot; a client keeps slot 0's, to the host. At its cap an outbox gives up its oldest blob that is not a
// busy level, which the typist's terminal needs to stop being busy.
struct Outbox {
    std::deque<std::vector<uint8_t>> q;
    Clock::time_point nextTry{};
    bool saidFull = false;
};
Outbox g_out[kMaxPeers];

coop::net::Session* SessionIf(coop::net::Role role) {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->role() == role) ? s : nullptr;
}

bool InSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running();
}

bool IsHost() { return SessionIf(coop::net::Role::Host) != nullptr; }

void Flush(uint8_t slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    Outbox& o = g_out[slot];
    while (!o.q.empty()) {
        if (!coop::blob_chunks::SendBlobToSlot(s, slot, ReliableKind::SatConsole, g_seq++, o.q.front())) {
            o.nextTry = Clock::now() + kResendEvery;
            return;
        }
        o.q.pop_front();
    }
    o.saidFull = false;
}

void Enqueue(uint8_t slot, std::vector<uint8_t> blob) {
    Outbox& o = g_out[slot];
    if (o.q.size() >= kOutboxCap) {
        for (auto it = o.q.begin(); it != o.q.end(); ++it) {
            if (!it->empty() && (*it)[0] != V::kOpBusy) {
                o.q.erase(it);
                break;
            }
        }
        if (!o.saidFull) {
            o.saidFull = true;
            UE_LOGW("sat_console: %zu blobs owed to slot %u and its sends still refused -- the oldest line dropped",
                    kOutboxCap, static_cast<unsigned>(slot));
        }
    }
    o.q.push_back(std::move(blob));
    if (Clock::now() >= o.nextTry) Flush(slot);
}

// A blob that states the whole of something (a screen, an input line) replaces one of its op AND terminal still owed:
// the older is never shown, so a slot whose sends are refused for a while owes one of each per terminal, not a
// backlog -- and one terminal's screen never displaces another's.
void EnqueueLatest(uint8_t slot, std::vector<uint8_t> blob) {
    Outbox& o = g_out[slot];
    uint8_t op = 0xFF;
    uint32_t term = V::kNoTerminal;
    if (V::PeekHeader(blob, op, term)) {
        o.q.erase(std::remove_if(o.q.begin(), o.q.end(),
                                 [op, term](const std::vector<uint8_t>& b) {
                                     uint8_t bop = 0;
                                     uint32_t bterm = 0;
                                     return V::PeekHeader(b, bop, bterm) && bop == op && bterm == term;
                                 }),
                  o.q.end());
    }
    Enqueue(slot, std::move(blob));
}

// ---- state: the host's executors ----

// HOST: the terminal a typist runs its lines on at one panel, keyed by its durable identity and that panel's terminal
// id: a typist that leaves mid-command leaves its executor running that command, as the game's terminal runs on when
// its player walks away, and one that comes back finds it again. An executor's prints belong to the terminal it was
// made for, whatever its typist does next. Kept while the session lasts and its world stays current; at the cap an
// idle executor gives way, so a typist may visit any number of panels.
struct Executor {
    std::string guid;
    TermId  term = 0;
    ue_wrap::GcPin pin;
    void*   world = nullptr;  // the world it was made in, when known
    int     slot = -1;        // its typist's slot while connected, -1 once it left
    bool    busySent = false; // the busy level its typist last got
    bool    haveContext = false;
    int32_t dish = V::kNoDish;
    std::wstring name;
    Clock::time_point lastUsed{};
};
std::vector<Executor> g_execs;

// A typist's line budget, over every terminal it types at.
struct Budget {
    int lines = 0;
    Clock::time_point windowStart{};
    Clock::time_point nextSpawnOk{};
};
std::unordered_map<std::string, Budget> g_budget;

bool g_saidRefused[kMaxPeers] = {};
bool g_saidMalformed[kMaxPeers] = {};
bool g_saidNoTerminal[kMaxPeers] = {};
bool g_saidNoContext[kMaxPeers] = {};

// Per client slot: the newest claim count, input sequence and presentation sequence taken from it. A client holds one
// terminal at a time, and its counters only move forward across terminals, so they are the slot's, not the terminal's.
uint32_t  g_slotStamp[kMaxPeers] = {};
uint32_t  g_slotInSeq[kMaxPeers] = {};
uint32_t  g_slotPresSeq[kMaxPeers] = {};
uint32_t  g_queuedPresSeq[kMaxPeers] = {};  // the newest present sequence taken into an owed queue, per slot
bool      g_owedSnapshot[kMaxPeers] = {};   // a slot told its world is ready: every terminal's screen is owed to it

// A client's line that arrived while its terminal's canonical screen was not up, kept unrun and in order; it runs when
// the terminal is, once, and is answered not ready after kLineWaitMax. Only a line that has not run is ever kept: a
// line that ran (or was answered) is gone from here, so no command is run twice.
struct WaitingLine {
    uint8_t slot = 0;
    V::Line line;
    Clock::time_point since{};
};

// A print, or a reset, owed to a terminal's canonical screen: written in the order it was asked, once it is up. An
// entry carries the present sequence it acknowledges, which is taken (acked) only as it is written.
struct Owed {
    bool         clear = false;
    std::wstring text;
    uint8_t      type = 0;
    int          ackSlot = -1;
    uint32_t     ackSeq = 0;
    int          attempts = 0;
};

// The holder's newest input, kept while the terminal is not up: applied (and acked) once it is.
struct HeldInput {
    bool     valid = false;
    uint8_t  slot = 0;
    V::Input in;
};

// ---- state: a terminal, both roles ----
//
// HOST: the canonical screen of one placed panel is a hidden terminal of the game's class (never constructed or shown,
// so no alert is bound to it) pointed at the panel's context; what was last published of it, who holds the panel and
// under which generation, and the readiness queues of lines, prints and input owed to it. CLIENT: the newest screen
// the host sent for it, kept whether or not a panel here is bound to the id yet, shown when this machine's console
// shows that terminal; and the busy level mirrored for it.
struct TermState {
    TermId       id = 0;
    std::wstring key;
    // host
    ue_wrap::GcPin canon;
    void*        canonWorld = nullptr;
    V::Screen    pub;
    bool         pubValid = false;
    uint32_t     rev = 0;
    uint32_t     gen = 0;
    uint8_t      holder = V::kNoHolder;
    bool         dirty = false;
    Clock::time_point nextScreen{};
    std::deque<WaitingLine> waiting;
    std::deque<Owed> owed;
    bool         saidOwedFull = false;
    HeldInput    heldInput;
    bool         saidNoCanon = false;
    // client
    bool         haveLast = false;
    V::Screen    last;
    bool         pending = false;  // `last` is not on the console yet
    Clock::time_point pendingSince{};
    bool         heldCounted = false;
    uint32_t     lastGen = 0;
    bool         mirrorBusy = false;
};
std::vector<std::unique_ptr<TermState>> g_terms;
bool g_saidTermsFull = false;

TermState* FindTerm(TermId id) {
    if (!id) return nullptr;
    for (auto& t : g_terms)
        if (t->id == id) return t.get();
    return nullptr;
}

TermState* GetTerm(TermId id) {
    if (TermState* t = FindTerm(id)) return t;
    if (!id) return nullptr;
    if (g_terms.size() >= kMaxTermStates) {
        if (!g_saidTermsFull) {
            g_saidTermsFull = true;
            UE_LOGW("sat_console: %zu terminals already have state here -- terminal %08X is not kept", kMaxTermStates, id);
        }
        return nullptr;
    }
    auto t = std::make_unique<TermState>();
    t->id = id;
    t->key = SC::KeyForId(id);
    g_terms.push_back(std::move(t));
    return g_terms.back().get();
}

TermState* ByCanon(void* obj) {
    if (!obj) return nullptr;
    for (auto& t : g_terms)
        if (t->canon.Raw() == obj) return t.get();
    return nullptr;
}

// ---- state: this machine's console ----
//
// Every panel shows the one console; it shows ONE terminal at a time: the one this machine's player last used or
// walked into, never a panel another player is at. `g_shown` is that terminal; a switch asked while a command this
// console runs itself is still going waits for it (its continuation reads the context it started with).
TermId       g_shown = 0;
TermId       g_wantShown = 0;
const char*  g_wantSource = "";
Clock::time_point g_nextShowTry{};
bool         g_seeded = false;     // HOST: the console's history from before the session went to its terminal
bool         g_localHolds = false; // a command this console runs itself holds its busy flag
TermId       g_localCmdTerm = 0;   // the terminal the console's own command started at
bool         g_pendingClear = false;  // the local line now running empties the screen

// The begin-overlap event running now: the panel and the actor that walked in.
struct OverlapNow {
    bool  active = false;
    void* panel = nullptr;
    void* actor = nullptr;
};
OverlapNow g_overlap;
int g_diagSaid = 0;

Clock::time_point g_holdUntil{};  // the desk's widget component ticks offscreen until this
bool      g_holding = false;
Clock::time_point g_nextPanels{};

// ---- state: the holder's input, host or client ----

bool      g_wasHolding = false;
TermId    g_holdTerm = 0;
uint32_t  g_myStamp = 0;     // this machine's count of its own claims of a terminal
uint32_t  g_inSeq = 0;
bool      g_inputDirty = false;
Clock::time_point g_nextInput{};
Clock::time_point g_nextPoll{};
std::wstring g_lastInputSent;
std::wstring g_lastNameSent;
int32_t   g_lastDishSent = V::kNoDish;
bool      g_ctxSentValid = false;

// ---- state: a client's ----

uint32_t g_presSeq = 0;     // the presentation sequence of the last print sent to the host
uint32_t g_presAcked = 0;   // the newest of them the host says it merged

// ---- the screen: reading ----

std::wstring ClampTail(const std::wstring& s, size_t n) {
    return s.size() > n ? s.substr(s.size() - n) : s;
}

int32_t DishIndex(void* d) {
    const int32_t i = d ? ue_wrap::dish::IndexOf(d) : V::kNoDish;
    return i < V::kNoDish ? V::kNoDish : i;
}

int32_t DishIndexOf(void* term) { return DishIndex(SC::ReadActiveDish(term)); }

bool ReadContext(void* term, std::wstring& name, int32_t& dish) {
    if (!SC::ReadName(term, name)) return false;
    if (name.size() > V::kMaxNameChars) name.resize(V::kMaxNameChars);
    dish = DishIndexOf(term);
    return true;
}

bool ReadHolderInput(void* term, std::wstring& text) {
    if (!SC::ReadInput(term, text)) return false;
    text = V::SanitizeInput(text);
    if (text.size() > V::kMaxInputChars) text.resize(V::kMaxInputChars);
    return true;
}

bool EqualNoCase(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// Whether a prompt and dish a client names are its panel's context here. A dish the host cannot index yet is no match.
enum class Match { Yes, No, NotYet };
Match ContextMatches(const SC::TerminalBinding& b, int32_t dish, const std::wstring& name) {
    if (!b.ready) return Match::NotYet;
    std::wstring want = b.name;
    if (want.size() > V::kMaxNameChars) want.resize(V::kMaxNameChars);
    const int32_t wantDish = b.dish ? ue_wrap::dish::IndexOf(b.dish) : V::kNoDish;
    if (b.dish && wantDish < 0) return Match::NotYet;
    return (wantDish == dish && EqualNoCase(want, name)) ? Match::Yes : Match::No;
}

bool LocalHoldsTerm(const TermState* t) { return t && DO::LocalHolds(t->key.c_str()); }

// ---- the host: canonical terminals ----

void DropCanon(TermState& t) {
    void* c = t.canon.Raw();
    t.canon.Release();
    SC::DiscardTerminal(c);
    t.canonWorld = nullptr;
}

// Say once per terminal, in the readiness trace's words, why its canonical screen is not up.
void ReportNoCanon(CR::Site site, TermState& t, const char* what) {
    SC::TerminalBinding b;
    const bool bound = SC::BindingForTerminal(t.id, b);
    CR::Report(site, !bound ? CR::Why::MissingActor : CR::Why::MissingWidget,
               "%s: terminal %08X (%ls) %s (panel %p '%ls', dish %p, prompt '%ls'), claim holder %u", what, t.id,
               t.key.c_str(), !bound ? "has no panel here" : (!b.ready ? "is not ready" : "could not be made"), b.panel,
               SC::PanelName(b.panel).c_str(), b.dish, Printable(b.name).c_str(),
               static_cast<unsigned>(DO::HolderOf(t.key.c_str())));
}

// The terminal's canonical screen, made (with `create`) once its panel is bound and its context ready: a terminal of
// the game's class, pinned, pointed at the desk as its panel and at the placed panel as `used`, init to the panel's
// context. One of a world no longer current is discarded first.
void* Canon(TermState& t, bool create) {
    if (t.canon.Raw()) {
        void* world = WI::CurrentWorld();
        if (!t.canonWorld || !world || t.canonWorld == world) return t.canon.Raw();
        DropCanon(t);
    }
    if (!create || !SC::EnsureResolved()) return nullptr;
    SC::TerminalBinding b;
    if (!SC::BindingForTerminal(t.id, b) || !b.ready) return nullptr;
    void* desk = ue_wrap::console_desk::Instance();
    void* term = desk ? SC::CreateTerminal(b.panel) : nullptr;
    if (!term || !t.canon.Pin(term) || !SC::WritePanel(term, desk) || !SC::WriteUsed(term, b.panel)) {
        t.canon.Release();
        SC::DiscardTerminal(term);
        return nullptr;
    }
    bool inited = false;
    {
        Applying guard;
        inited = SC::CallInit(term, b.name, false, b.dish);
    }
    if (!inited) {
        Note(Reason::NoContext, b.name);
        DropCanon(t);
        return nullptr;
    }
    t.canonWorld = WI::CurrentWorld();
    t.dirty = true;
    t.saidNoCanon = false;
    UE_LOGI("sat_console: HOST terminal %08X '%ls' (panel '%ls') has its canonical screen %p", t.id,
            Printable(b.name).c_str(), SC::PanelName(b.panel).c_str(), term);
    return term;
}

Executor* ExecByObject(void* obj) {
    if (!obj) return nullptr;
    for (Executor& e : g_execs)
        if (e.pin.Raw() == obj) return &e;
    return nullptr;
}

void SendBusy(uint8_t slot, TermId term, bool busy) {
    V::Busy v;
    v.term = term;
    v.busy = busy;
    std::vector<uint8_t> b;
    V::EncodeBusy(b, v);
    Enqueue(slot, std::move(b));
}

// Every peer that has a connection and a world: where the screens go.
template <class F>
void ForEachClient(F&& f) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    for (int slot = 1; slot < kMaxPeers; ++slot)
        if (s->HasPeerConn(slot) && s->IsSlotWorldReady(slot)) f(static_cast<uint8_t>(slot));
}

// A command running at a terminal for any typist.
bool ExecBusy(const TermState& t) {
    for (const Executor& e : g_execs) {
        bool busy = false;
        if (e.term == t.id && SC::ReadProcessing(e.pin.Raw(), busy) && busy) return true;
    }
    return false;
}

// The busy level a terminal's screen shows: a command running there for any typist, or the host's console's own.
bool TermBusy(const TermState& t) { return ExecBusy(t) || (t.id == g_shown && g_localHolds); }

// A terminal's canonical screen, read after the game's own append and trim.
bool HostBuildScreen(TermState& t, void* canon, V::Screen& out) {
    std::wstring log;
    if (!SC::ReadLog(canon, log) || !ReadContext(canon, out.name, out.dish)) return false;
    out.log = ClampTail(log, V::kMaxHistory);
    if (!ReadHolderInput(canon, out.input)) return false;
    out.term = t.id;
    out.busy = TermBusy(t) ? 1 : 0;
    out.gen = t.gen;
    out.holder = t.holder;
    return true;
}

void SendScreenTo(uint8_t slot, const V::Screen& base) {
    V::Screen s = base;
    s.ackInput = g_slotInSeq[slot];
    s.ackPresent = g_slotPresSeq[slot];
    std::vector<uint8_t> b;
    V::EncodeScreen(b, s);
    EnqueueLatest(slot, std::move(b));
    ++g_counts.screensSent;
}

bool ApplyScreen(void* term, const V::Screen& s, bool holding, TermState& t);

// The host's console is a replica of the canonical screen of the terminal it shows.
void HostShowLocal(TermState& t, const V::Screen& s) {
    if (t.id != g_shown) return;
    void* w = SC::LocalTerminal();
    if (!w) return;
    // The console's own command is not a mirror of itself: its flag is the command's.
    V::Screen local = s;
    local.busy = ExecBusy(t) ? 1 : 0;
    ApplyScreen(w, local, LocalHoldsTerm(&t), t);
}

// Publish a terminal's screen when it moved: the history, the prompt or the busy level (the input rides its own
// packets). False when it could not be read or published, so the caller keeps it dirty and tries again; true when it
// was sent or had not moved, or the terminal has no canonical screen yet (nothing to show).
bool HostPublish(TermState& t, bool force) {
    void* canon = Canon(t, true);
    if (!canon) {
        if (t.owed.empty() && t.waiting.empty() && !t.heldInput.valid) return true;  // nothing printed there yet
        Note(Reason::NoScreenTerminal);
        if (!t.saidNoCanon) {
            t.saidNoCanon = true;
            ReportNoCanon(CR::Site::SatPublish, t, "the screen cannot be published");
        }
        return false;
    }
    V::Screen cur;
    if (!HostBuildScreen(t, canon, cur)) {
        CR::Report(CR::Site::SatPublish, CR::Why::DispatchFailed,
                   "terminal %08X's canonical screen %p: its log, prompt or input could not be read", t.id, canon);
        return false;
    }
    const bool changed = force || !t.pubValid || cur.log != t.pub.log || cur.name != t.pub.name ||
                         cur.dish != t.pub.dish || cur.busy != t.pub.busy || cur.holder != t.pub.holder ||
                         cur.gen != t.pub.gen || cur.input != t.pub.input;
    if (!changed) return true;
    cur.rev = ++t.rev;
    t.pub = cur;
    t.pubValid = true;
    size_t peers = 0;
    ForEachClient([&](uint8_t slot) {
        SendScreenTo(slot, cur);
        ++peers;
    });
    HostShowLocal(t, cur);
    if (Trace())
        UE_LOGI("sat_console: HOST terminal %08X screen rev %u to %zu peer(s): %zu chars, prompt '%ls' dish %d, busy %d, "
                "holder %u gen %u", t.id, cur.rev, peers, cur.log.size(), Printable(cur.name).c_str(), cur.dish,
                static_cast<int>(cur.busy), static_cast<unsigned>(cur.holder), cur.gen);
    return true;
}

// A late joiner, or a peer whose world just became ready: every terminal's screen there is, even if nothing moved.
void SendSnapshot(uint8_t slot) {
    g_owedSnapshot[slot] = false;
    size_t sent = 0, chars = 0;
    for (size_t i = 0; i < g_terms.size(); ++i) {
        TermState& t = *g_terms[i];
        void* canon = Canon(t, false);
        V::Screen cur;
        if (!canon || !HostBuildScreen(t, canon, cur)) continue;
        cur.rev = t.rev;
        SendScreenTo(slot, cur);
        ++sent;
        chars += cur.log.size();
    }
    UE_LOGI("sat_console: HOST sent slot %u every terminal's screen (%zu terminal(s), %zu chars)",
            static_cast<unsigned>(slot), sent, chars);
}

// What a typist whose terminal's screen cannot be published is told: that terminal's last published screen with one
// line more. Best effort -- the terminal that would hold the canonical text is not up -- and the lines owed to it are
// written in their order when it is, so the notice is in the history then.
void SendNoticeScreen(uint8_t slot, TermState& t, const V::Line& in, const wchar_t* text) {
    V::Screen s = t.pubValid ? t.pub : V::Screen{};
    s.term = t.id;
    s.log = ClampTail(s.log + (s.log.empty() ? std::wstring() : std::wstring(L"\n")) + text, V::kMaxHistory);
    s.name = in.name;
    s.dish = in.dish;
    s.input.clear();
    s.busy = 0;
    s.holder = t.holder;
    s.gen = t.gen;
    s.rev = t.rev;
    SendScreenTo(slot, s);
}

// Write one owed entry to a canonical screen; false when the call did not run.
bool ApplyOwed(void* canon, const Owed& o) {
    if (o.clear) {
        Applying guard;
        return SC::WriteLog(canon, std::wstring());
    }
    return SC::CallWriteToLog(canon, o.text, o.type);
}

// An entry written: the screen moved, and the print it acknowledged is taken.
void FinishOwed(TermState& t, const Owed& o) {
    t.dirty = true;
    if (o.ackSlot >= 0 && o.ackSlot < kMaxPeers) {
        g_slotPresSeq[o.ackSlot] = std::max(g_slotPresSeq[o.ackSlot], o.ackSeq);
        ++g_counts.presentsMerged;
        if (Trace())
            UE_LOGI("sat_console: HOST merged slot %d's %s (seq %u) into terminal %08X", o.ackSlot,
                    o.clear ? "clr" : "print", o.ackSeq, t.id);
    }
    if (CR::TraceOn())
        CR::Report(CR::Site::SatMerge, CR::Why::OutputMerged,
                   "%s '%ls' type %u written to terminal %08X, claim holder %u", o.clear ? "reset" : "print",
                   Printable(o.text).c_str(), static_cast<unsigned>(o.type), t.id,
                   static_cast<unsigned>(DO::HolderOf(t.key.c_str())));
}

// Write the owed entries in their order while the canonical screen is up; one whose call does not run stays at the
// head, tried again, and dropped (said) after kOwedAttempts runs of the call refused.
void FlushOwed(TermState& t) {
    if (t.owed.empty()) return;
    void* canon = Canon(t, true);
    if (!canon) return;
    while (!t.owed.empty()) {
        Owed& o = t.owed.front();
        if (ApplyOwed(canon, o)) {
            FinishOwed(t, o);
            t.owed.pop_front();
            continue;
        }
        if (++o.attempts >= kOwedAttempts) {
            CR::Report(CR::Site::SatMerge, CR::Why::DispatchFailed,
                       "terminal %08X's canonical screen %p refused a %s %d times -- dropped: '%ls'", t.id, canon,
                       o.clear ? "reset" : "print", o.attempts, Printable(o.text).c_str());
            t.owed.pop_front();
            continue;
        }
        return;
    }
}

// A print or reset for a terminal's screen: written at once while nothing is owed and it is up, else kept in order
// behind what is owed and written when it is. Never lost while the session lasts, within the cap.
void SubmitOwed(TermState& t, Owed o) {
    if (t.owed.empty()) {
        if (void* canon = Canon(t, true)) {
            if (ApplyOwed(canon, o)) {
                FinishOwed(t, o);
                return;
            }
            ++o.attempts;
            CR::Report(CR::Site::SatMerge, CR::Why::DispatchFailed,
                       "terminal %08X's canonical screen %p: writeToLog did not run for '%ls' -- kept, tried again", t.id,
                       canon, Printable(o.text).c_str());
        } else {
            Note(Reason::NoScreenTerminal);
            if (!t.saidNoCanon) {
                t.saidNoCanon = true;
                ReportNoCanon(CR::Site::SatMerge, t, "a print is kept for the terminal");
            }
        }
    }
    if (t.owed.size() >= kOwedCap) {
        if (!t.saidOwedFull) {
            t.saidOwedFull = true;
            UE_LOGW("sat_console: %zu prints are owed to terminal %08X and it is still not up -- the oldest dropped",
                    kOwedCap, t.id);
        }
        t.owed.pop_front();
    }
    Note(Reason::PrintKept);
    t.owed.push_back(std::move(o));
    FlushOwed(t);
}

// A line printed for a terminal is on its screen: written to its canonical terminal, whose own append and trim make
// it the canonical text. While that terminal is not up the line is kept and written in its order when it is.
bool MergeToScreen(TermState& t, const std::wstring& text, uint8_t type) {
    Owed o;
    o.text = text.size() > V::kMaxPrintChars ? text.substr(0, V::kMaxPrintChars) : text;
    o.type = type;
    SubmitOwed(t, std::move(o));
    return true;
}

bool MergeToScreen(TermId id, const std::wstring& text, uint8_t type) {
    TermState* t = GetTerm(id);
    return t && MergeToScreen(*t, text, type);
}

// A line the host will not run is answered as the game answers one it cannot take or run: its echo, then the
// reason's line, on that terminal's screen. The typist already consumed it. With the terminal not up the answer is
// also sent to the typist as a screen of its own, so it is not left waiting on one.
void AnswerRefused(uint8_t slot, TermState& t, const V::Line& in, const wchar_t* why) {
    MergeToScreen(t, in.line, 1);
    MergeToScreen(t, why, 0);
    if (!Canon(t, false)) SendNoticeScreen(slot, t, in, why);
}

// Unpin an executor and stop what it runs; its typist, if still here, is told it is no longer busy.
void Discard(Executor& e) {
    void* term = e.pin.Raw();
    if (e.slot >= 0 && e.busySent) SendBusy(static_cast<uint8_t>(e.slot), e.term, false);
    e.pin.Release();
    SC::DiscardTerminal(term);
}

void DiscardExecAt(size_t i) {
    Discard(g_execs[i]);
    g_execs.erase(g_execs.begin() + static_cast<std::ptrdiff_t>(i));
}

// A typist back at an executor it left: bound to it again and told its busy level, so its own console is busy at that
// terminal while the command it left runs on, and that command's lines keep going to that terminal's screen.
void Rebind(Executor& e, uint8_t slot) {
    e.slot = slot;
    bool busy = false;
    SC::ReadProcessing(e.pin.Raw(), busy);
    e.busySent = busy;
    SendBusy(slot, e.term, busy);
    ++g_counts.rebinds;
    if (busy) ++g_counts.busyRebinds;
    UE_LOGI("sat_console: HOST slot %u is back at its executor for terminal %08X (busy=%d)", static_cast<unsigned>(slot),
            e.term, busy ? 1 : 0);
}

// The typist's executor at a terminal, made on its first line there. At the cap the idle executor used longest ago
// gives way (a departed typist's first); with every one busy, the line is refused.
Executor* ExecutorFor(uint8_t slot, const std::string& guid, TermId term, void* worldContext) {
    const auto now = Clock::now();
    for (Executor& e : g_execs) {
        if (e.guid != guid || e.term != term) continue;
        if (e.slot != slot) Rebind(e, slot);
        e.lastUsed = now;
        return &e;
    }
    if (g_execs.size() >= kMaxExecutors) {
        size_t pick = g_execs.size();
        for (int pass = 0; pass < 2 && pick == g_execs.size(); ++pass) {
            for (size_t i = 0; i < g_execs.size(); ++i) {
                bool busy = true;
                if (pass == 0 && g_execs[i].slot >= 0) continue;  // departed typists' first
                if (!SC::ReadProcessing(g_execs[i].pin.Raw(), busy) || busy) continue;
                if (pick == g_execs.size() || g_execs[i].lastUsed < g_execs[pick].lastUsed) pick = i;
            }
        }
        if (pick == g_execs.size()) return nullptr;
        UE_LOGI("sat_console: HOST executor for terminal %08X gives way (idle, %zu kept)", g_execs[pick].term, g_execs.size());
        DiscardExecAt(pick);
    }
    void* desk = ue_wrap::console_desk::Instance();
    void* made = desk ? SC::CreateTerminal(worldContext) : nullptr;
    Executor e;
    if (!made || !e.pin.Pin(made) || !SC::WritePanel(made, desk)) {
        e.pin.Release();
        SC::DiscardTerminal(made);
        return nullptr;
    }
    e.guid = guid;
    e.term = term;
    e.world = WI::CurrentWorld();  // null while unknown: the world term then fails open
    e.slot = slot;
    e.lastUsed = now;
    g_execs.push_back(std::move(e));
    UE_LOGI("sat_console: HOST made slot %u's executor for terminal %08X (%zu kept)", static_cast<unsigned>(slot), term,
            g_execs.size());
    return &g_execs.back();
}

// Whether an executor stands in its panel's context. A context whose init fails answers the line err rather than
// running it against none; nothing is kept, so the next line tries again.
bool ApplyContext(Executor& e, const SC::TerminalBinding& b) {
    const int32_t dish = DishIndex(b.dish);
    if (e.haveContext && dish == e.dish && b.name == e.name) return true;
    e.haveContext = false;
    if (!SC::CallInit(e.pin.Raw(), b.name, false, b.dish)) return false;
    e.haveContext = true;
    e.dish = dish;
    e.name = b.name;
    return true;
}

void HostRunLine(uint8_t slot, const V::Line& in, TermState& t);

// A terminal's kept lines, run in the order they came, once its canonical screen is up. Each is taken off the queue
// before it runs, so none can run twice.
void DrainWaiting(TermState& t) {
    while (!t.waiting.empty()) {
        WaitingLine w = std::move(t.waiting.front());
        t.waiting.pop_front();
        HostRunLine(w.slot, w.line, t);
    }
}

// Each tick: run a terminal's kept lines if it is up; else answer those that waited kLineWaitMax not ready, on its
// screen (kept for it) and to the typist directly.
void TickWaiting(TermState& t, Clock::time_point now) {
    if (t.waiting.empty()) return;
    if (Canon(t, true)) {
        DrainWaiting(t);
        return;
    }
    while (!t.waiting.empty() && now - t.waiting.front().since >= kLineWaitMax) {
        WaitingLine w = std::move(t.waiting.front());
        t.waiting.pop_front();
        Note(Reason::LineNotReady, w.line.line);
        ReportNoCanon(CR::Site::SatLine, t, "the line waited for the terminal and is answered not ready");
        AnswerRefused(w.slot, t, w.line, kNotReadyLine);
    }
}

// A client's line at a terminal whose canonical screen is up: run on the typist's executor there, answered, or
// refused. Whatever this does with the line is final -- it is run once or answered, never kept to run later.
void HostRunLine(uint8_t slot, const V::Line& in, TermState& t) {
    const std::wstring& line = in.line;
    SC::TerminalBinding b;
    if (!SC::BindingForTerminal(t.id, b) || !b.ready) {
        Note(Reason::NoContext, line);
        AnswerRefused(slot, t, in, kErrLine);
        return;
    }
    // The line runs only in its panel's context: a client whose prompt is another panel's is not run here.
    const Match m = ContextMatches(b, in.dish, in.name);
    if (m != Match::Yes) {
        Note(Reason::ContextMismatch, line);
        UE_LOGW("sat_console: HOST slot %u's line at terminal %08X names prompt '%ls' dish %d; the panel '%ls' here is "
                "'%ls' dish %d -- answered err", static_cast<unsigned>(slot), t.id, Printable(in.name).c_str(), in.dish,
                SC::PanelName(b.panel).c_str(), Printable(b.name).c_str(), DishIndex(b.dish));
        AnswerRefused(slot, t, in, kErrLine);
        return;
    }
    const std::string& guid = coop::player_handshake::GuidForSlot(slot);
    Executor* e = guid.empty() ? nullptr : ExecutorFor(slot, guid, t.id, b.panel);
    if (!e) {
        if (!g_saidNoTerminal[slot]) {
            g_saidNoTerminal[slot] = true;
            UE_LOGW("sat_console: HOST no executor for slot %u (%s) -- its lines are answered err",
                    static_cast<unsigned>(slot), guid.empty() ? "its identity not in yet" : "none could be made");
        }
        AnswerRefused(slot, t, in, guid.empty() ? kErrLine : kBusyLine);
        return;
    }
    const auto now = Clock::now();
    Budget& bud = g_budget[guid];
    if (now - bud.windowStart >= kWindow) {
        bud.windowStart = now;
        bud.lines = 0;
    }
    const bool spawns = T::SpawnsEveryCall(line);
    if (++bud.lines > kLinesPerWindow || (spawns && now < bud.nextSpawnOk)) {
        AnswerRefused(slot, t, in, kBusyLine);  // the terminal cannot take the line, as the game says it
        return;
    }
    // An executor still running a command keeps its context: its latent continuation reads the dish, name and panel
    // it started with. The game's own terminal refuses a line while busy, so does this.
    bool busy = false;
    const bool readOk = SC::ReadProcessing(e->pin.Raw(), busy);
    if (!readOk || busy) {
        AnswerRefused(slot, t, in, readOk ? kBusyLine : kErrLine);   // an unreadable flag is not "busy"
        return;
    }
    if (!ApplyContext(*e, b) || !SC::WriteUsed(e->pin.Raw(), b.panel)) {
        if (!g_saidNoContext[slot]) {
            g_saidNoContext[slot] = true;
            UE_LOGW("sat_console: HOST no context for slot %u's line at terminal %08X -- answered err",
                    static_cast<unsigned>(slot), t.id);
        }
        AnswerRefused(slot, t, in, kErrLine);
        return;
    }
    if (spawns) bud.nextSpawnOk = now + kSpawnEvery;
    UE_LOGI("sat_console: HOST runs '%ls' for slot %u at terminal %08X '%ls' (dish %d)", Printable(line).c_str(),
            static_cast<unsigned>(slot), t.id, Printable(b.name).c_str(), in.dish);
    void* canon = Canon(t, false);
    std::wstring logBefore, logAfter;
    const bool haveBefore = canon && SC::ReadLog(canon, logBefore);
    const size_t owedBefore = t.owed.size();
    bool busyBefore = false, busyAfter = false;
    SC::ReadProcessing(e->pin.Raw(), busyBefore);
    if (!SC::RunCommand(e->pin.Raw(), line)) {
        CR::Report(CR::Site::SatLine, CR::Why::DispatchFailed,
                   "slot %u '%ls': enterCommand did not run on executor %p (terminal %08X, canonical %p, claim holder %u)",
                   static_cast<unsigned>(slot), Printable(line).c_str(), e->pin.Raw(), t.id, canon,
                   static_cast<unsigned>(DO::HolderOf(t.key.c_str())));
        AnswerRefused(slot, t, in, kErrLine);
        return;
    }
    ++g_counts.linesRun;
    SC::ReadProcessing(e->pin.Raw(), busyAfter);
    const bool haveAfter = canon && SC::ReadLog(canon, logAfter);
    const bool printed = (haveBefore && haveAfter && logAfter != logBefore) || t.owed.size() > owedBefore;
    const CR::Why why = printed ? CR::Why::OutputMerged
                                : (busyAfter ? CR::Why::WaitingContinuation : CR::Why::NativeNoOp);
    CR::Report(CR::Site::SatLine, why,
               "slot %u '%ls' at terminal %08X: executor %p busy %d->%d, canonical %p log %s%s, claim holder %u, %zu "
               "print(s) owed%s", static_cast<unsigned>(slot), Printable(line).c_str(), t.id, e->pin.Raw(),
               busyBefore ? 1 : 0, busyAfter ? 1 : 0, canon, printed ? "moved" : "unchanged",
               busyAfter ? ", a continuation runs" : "", static_cast<unsigned>(DO::HolderOf(t.key.c_str())),
               t.owed.size(),
               why == CR::Why::NativeNoOp ? " (it printed nothing and left nothing running: a world effect only, or a no-op)" : "");
}

// A client's line. Refused at once when it is not one the host runs, or names a terminal another peer holds; run when
// its terminal's canonical screen is up; else kept, unrun and in order, and run when it is, or answered not ready.
void HostOnLine(uint8_t slot, const V::Line& in) {
    // The host runs only a command a typist may send: a line the table keeps on its typist's machine
    // would run on the host's own compass, player or screen. A client that sends one was not built to.
    if (T::Classify(in.line) != T::Runs::OnHost) {
        if (!g_saidRefused[slot]) {
            g_saidRefused[slot] = true;
            UE_LOGW("sat_console: HOST refused '%ls' from slot %u -- it runs on its typist's machine",
                    Printable(in.line).c_str(), static_cast<unsigned>(slot));
        }
        return;
    }
    SC::TerminalBinding b;
    const bool bound = SC::BindingForTerminal(in.term, b);
    if (bound) {
        const uint8_t holder = DO::HolderOf(b.key.c_str());
        if (holder != V::kNoHolder && holder != slot) {
            Note(Reason::NotHolder, in.line);
            return;
        }
    }
    TermState* t = GetTerm(in.term);
    if (!t) {
        Note(Reason::UnknownTerminal, in.line);
        return;
    }
    if (bound && Canon(*t, true)) {
        DrainWaiting(*t);  // lines kept before this one run first: a typist's lines keep their order
        HostRunLine(slot, in, *t);
        return;
    }
    size_t mine = 0;
    for (auto& tp : g_terms)
        for (const WaitingLine& w : tp->waiting)
            if (w.slot == slot) ++mine;
    if (mine >= kMaxWaitingPerSlot) {
        AnswerRefused(slot, *t, in, kBusyLine);
        return;
    }
    Note(bound ? Reason::LineKept : Reason::UnknownTerminal, in.line);
    ReportNoCanon(CR::Site::SatLine, *t, "the line is kept, unrun, until the terminal is up");
    WaitingLine w;
    w.slot = slot;
    w.line = in;
    w.since = Clock::now();
    t->waiting.push_back(std::move(w));
}

bool InputNewer(const V::Input& a, const V::Input& b) {
    return a.stamp > b.stamp || (a.stamp == b.stamp && a.seq > b.seq);
}

// The holder's input, put on its terminal's canonical screen, on the host's console if it shows that terminal, and
// relayed to the others; taken (acked) here and no sooner.
void HostApplyInput(uint8_t slot, const V::Input& in, TermState& t, void* canon) {
    g_slotStamp[slot] = in.stamp;
    g_slotInSeq[slot] = in.seq;
    const std::wstring text = V::SanitizeInput(in.text);
    std::wstring cur;
    if (!ReadHolderInput(canon, cur) || cur != text) {
        Applying guard;
        if (!SC::WriteInput(canon, text))
            CR::Report(CR::Site::SatInput, CR::Why::DispatchFailed,
                       "slot %u's %zu-char input could not be written to terminal %08X's canonical screen %p",
                       static_cast<unsigned>(slot), text.size(), t.id, canon);
        else if (CR::TraceOn())
            CR::Report(CR::Site::SatInput, CR::Why::StateChanged,
                       "slot %u's %zu-char input written to terminal %08X, claim holder %u", static_cast<unsigned>(slot),
                       text.size(), t.id, static_cast<unsigned>(DO::HolderOf(t.key.c_str())));
    }
    if (t.id == g_shown && !LocalHoldsTerm(&t)) {
        if (void* w = SC::LocalTerminal()) {
            std::wstring shown;
            if (!ReadHolderInput(w, shown) || shown != text) {
                Applying guard;
                SC::WriteInput(w, text);
            }
        }
    }
    if (t.pubValid) t.pub.input = text;
    ++g_counts.inputsTaken;
    V::InputRelay r;
    r.term = t.id;
    r.holder = slot;
    r.gen = t.gen;
    r.seq = in.seq;
    r.text = text;
    ForEachClient([&](uint8_t to) {
        if (to == slot) return;
        std::vector<uint8_t> b;
        V::EncodeInputRelay(b, r);
        EnqueueLatest(to, std::move(b));
    });
    if (Trace())
        UE_LOGI("sat_console: HOST input from slot %u at terminal %08X: stamp %u seq %u, %zu chars",
                static_cast<unsigned>(slot), t.id, in.stamp, in.seq, text.size());
}

// The holder's input line. Refused unless the sender is the claim table's holder of that panel, its context is the
// panel's, and it is newer than the last taken from that slot. While the terminal is not up the newest is kept and
// put on it when it is, so the sequence is acked only after it is applied.
void HostOnInput(uint8_t slot, const V::Input& in) {
    SC::TerminalBinding b;
    if (!SC::BindingForTerminal(in.term, b)) {
        Note(Reason::UnknownTerminal);
        return;
    }
    if (DO::HolderOf(b.key.c_str()) != slot) {
        ++g_counts.inputsStale;
        Note(Reason::NotHolder);
        return;
    }
    if (in.stamp < g_slotStamp[slot] || (in.stamp == g_slotStamp[slot] && in.seq <= g_slotInSeq[slot])) {
        ++g_counts.inputsStale;
        Note(Reason::StaleInput);
        return;
    }
    if (ContextMatches(b, in.dish, in.name) == Match::No) {
        ++g_counts.inputsStale;
        Note(Reason::ContextMismatch, in.name);
        return;
    }
    TermState* t = GetTerm(in.term);
    if (!t) return;
    void* canon = b.ready ? Canon(*t, true) : nullptr;
    if (!canon) {
        Note(Reason::NoScreenTerminal);
        if (!t->heldInput.valid || t->heldInput.slot != slot || InputNewer(in, t->heldInput.in)) {
            t->heldInput.valid = true;
            t->heldInput.slot = slot;
            t->heldInput.in = in;
        }
        Note(Reason::InputKept);
        ReportNoCanon(CR::Site::SatInput, *t, "the holder's input is kept for the terminal");
        return;
    }
    t->heldInput.valid = false;  // this one is newer than anything kept
    HostApplyInput(slot, in, *t, canon);
}

// A terminal's kept input, applied once it is up if its sender still holds the panel and nothing newer from it was
// taken meanwhile.
void TickHeldInput(TermState& t) {
    if (!t.heldInput.valid) return;
    if (DO::HolderOf(t.key.c_str()) != t.heldInput.slot) {
        t.heldInput.valid = false;
        return;
    }
    void* canon = Canon(t, true);
    if (!canon) return;
    const HeldInput h = t.heldInput;
    t.heldInput.valid = false;
    if (h.in.stamp < g_slotStamp[h.slot] || (h.in.stamp == g_slotStamp[h.slot] && h.in.seq <= g_slotInSeq[h.slot])) return;
    HostApplyInput(h.slot, h.in, t, canon);
}

// What a command the holder ran at its own console printed, written to that terminal's screen in the order the typist
// saw it. `clr` is the terminal's reset. Kept, in order, while the terminal is not up; its sequence is acked when it is
// written.
void HostOnPresent(uint8_t slot, const V::Present& p) {
    SC::TerminalBinding b;
    if (!SC::BindingForTerminal(p.term, b)) {
        Note(Reason::UnknownTerminal);
        return;
    }
    if (DO::HolderOf(b.key.c_str()) != slot) {
        Note(Reason::NotHolder);
        return;
    }
    if (p.seq <= std::max(g_slotPresSeq[slot], g_queuedPresSeq[slot])) {
        Note(Reason::StaleInput);
        return;
    }
    TermState* t = GetTerm(p.term);
    if (!t) return;
    g_queuedPresSeq[slot] = p.seq;
    Owed o;
    o.clear = p.kind != V::PresentKind::Write;
    if (!o.clear) {
        o.text = p.text.size() > V::kMaxPrintChars ? p.text.substr(0, V::kMaxPrintChars) : p.text;
        o.type = p.type;
    }
    o.ackSlot = slot;
    o.ackSeq = p.seq;
    SubmitOwed(*t, std::move(o));
}

void HostOnBlob(uint8_t slot, const std::vector<uint8_t>& blob) {
    uint8_t op = 0;
    uint32_t term = 0;
    V::Line line;
    V::Input input;
    V::Present present;
    const bool ok = V::PeekHeader(blob, op, term);
    if (ok && op == V::kOpLine) {
        if (V::DecodeLine(blob, line)) {
            HostOnLine(slot, line);
            return;
        }
    } else if (ok && op == V::kOpInput) {
        if (V::DecodeInput(blob, input)) {
            HostOnInput(slot, input);
            return;
        }
    } else if (ok && op == V::kOpPresent) {
        if (V::DecodePresent(blob, present)) {
            HostOnPresent(slot, present);
            return;
        }
    } else if (ok) {
        Note(Reason::WrongDirection);
        return;
    }
    if (!g_saidMalformed[slot]) {
        g_saidMalformed[slot] = true;
        UE_LOGW("sat_console: HOST a malformed blob from slot %u -- dropped", static_cast<unsigned>(slot));
    }
    Note(Reason::BadPacket);
}

// Every write of a terminal's busy flag lies in its enterCommand or its ubergraph, the latent
// continuations included, so the level is read after each and sent when it moved.
void HostAfterBody(Executor& e) {
    if (TermState* t = FindTerm(e.term)) t->dirty = true;  // its busy level is part of the screen
    bool busy = false;
    if (e.slot < 0 || !SC::ReadProcessing(e.pin.Raw(), busy) || busy == e.busySent) return;
    e.busySent = busy;
    SendBusy(static_cast<uint8_t>(e.slot), e.term, busy);
}

// ---- this machine's console: which terminal it shows ----

bool ApplyScreen(void* term, const V::Screen& s, bool holding, TermState& t) {
    Applying guard;
    bool ok = true;
    std::wstring curLog;
    if (!SC::ReadLog(term, curLog) || curLog != s.log) ok = SC::WriteLog(term, s.log) && ok;
    if (!holding) {
        std::wstring curIn;
        const std::wstring input = V::SanitizeInput(s.input);
        if (!ReadHolderInput(term, curIn) || curIn != input) ok = SC::WriteInput(term, input) && ok;
        t.mirrorBusy = s.busy != 0;
        if (!g_localHolds) SC::WriteProcessing(term, t.mirrorBusy);
    }
    return ok;
}

void SayDecision(const char* fmt, ...);

// Show terminal `id` on this machine's console: its panel's prompt (init, ours), then its screen -- the canonical one
// on the host, the newest the host sent on a client, an empty one when nothing was printed there yet. A switch asked
// while the console runs a command of its own waits for it. A panel whose context is not ready is not shown (the
// console keeps its terminal), never ROOT in its place.
bool ShowTerminal(TermId id, const char* source) {
    void* w = SC::LocalTerminal();
    if (!w || !id) return false;
    SC::TerminalBinding b;
    if (!SC::BindingForTerminal(id, b) || !b.ready) {
        Note(Reason::NoContext, SC::KeyForId(id));
        SayDecision("terminal %08X (%s) is not ready (panel %p, dish %p, prompt '%ls') -- the console keeps terminal %08X",
                    id, source, b.panel, b.dish, Printable(b.name).c_str(), g_shown);
        return false;
    }
    if (g_localHolds && id != g_shown) {
        g_wantShown = id;
        g_wantSource = source;
        SayDecision("terminal %08X (%s) waits: the console's own command at terminal %08X still runs", id, source, g_shown);
        return true;
    }
    TermState* t = GetTerm(id);
    if (!t) return false;
    std::wstring oldName;
    SC::ReadName(w, oldName);
    bool inited = false;
    {
        Applying guard;
        inited = SC::CallInit(w, b.name, false, b.dish);
    }
    if (!inited) {
        Note(Reason::NoContext, b.name);
        return false;
    }
    const TermId prev = g_shown;
    g_shown = id;
    g_wantShown = 0;
    V::Screen s;
    bool have = false;
    if (IsHost()) {
        if (void* canon = Canon(*t, false)) have = HostBuildScreen(*t, canon, s);
        if (have) s.busy = ExecBusy(*t) ? 1 : 0;
    } else if (t->haveLast) {
        s = t->last;
        have = true;
        t->pending = false;
    }
    if (!have) {
        s = V::Screen{};
        s.term = id;
    }
    ApplyScreen(w, s, LocalHoldsTerm(t), *t);
    g_wasHolding = false;  // the input baseline belongs to the terminal shown
    UE_LOGI("sat_console: this machine's console shows terminal %08X '%ls' (panel '%ls', %s; was %08X '%ls')%s", id,
            Printable(b.name).c_str(), SC::PanelName(b.panel).c_str(), source, prev, Printable(oldName).c_str(),
            have ? "" : " -- nothing printed there yet");
    return true;
}

// Where this machine's widget chain breaks, in the readiness trace's words: the actor and each link.
void ReportNoScreen(CR::Site site, const char* what) {
    const SC::ScreenDiag d = SC::DiagnoseLocal();
    CR::Report(site, d.status == SC::ScreenStatus::NoDesk ? CR::Why::MissingActor : CR::Why::MissingWidget,
               "%s: %s (desk %p, desk widget %p, terminal %p, desk widget component %p%s), shown terminal %08X", what,
               SC::StatusText(d.status), d.desk, d.atlas, d.terminal, d.widgetComponent,
               d.componentHoldsAtlas ? " holding the atlas" : "", g_shown);
}

// The terminal the console shows when a session finds it: the panel last used, else the desk's root panel. On the
// host its history from before the session becomes that terminal's.
void InitialShow(Clock::time_point now) {
    if (now < g_nextShowTry) return;
    g_nextShowTry = now + kShowRetry;
    void* w = SC::LocalTerminal();
    if (!w) {
        ReportNoScreen(CR::Site::SatPublish, "this machine's console is not up to show a terminal");
        return;
    }
    void* panel = SC::ReadUsed(w);
    if (!SC::IsPanel(panel)) panel = SC::RootPanel();
    const TermId id = SC::PanelId(panel);
    if (!id) return;
    if (IsHost() && !g_seeded) {
        TermState* t = GetTerm(id);
        std::wstring log;
        if (!t || !SC::ReadLog(w, log)) return;
        if (!log.empty()) {
            void* canon = Canon(*t, true);
            if (!canon) return;  // tried again: the panel's context is not ready yet
            Applying guard;
            SC::WriteLog(canon, ClampTail(log, V::kMaxHistory));
            t->dirty = true;
        }
        g_seeded = true;
    }
    ShowTerminal(id, "session start");
}

void SayDecision(const char* fmt, ...) {
    if (g_diagSaid >= kDiagSay && !Trace()) return;
    ++g_diagSaid;
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    UE_LOGI("sat_console: %s%s", buf, g_diagSaid == kDiagSay && !Trace() ? " (further decisions under the trace)" : "");
}

// ---- the holder: typing ----

// The prompt and the text a holder shows, sent to the host (a client) or put on the canonical screen and relayed to
// the rest (the host) when either moved. At the claim's rising edge the console already shows what the screen showed,
// so that is the baseline and nothing is sent until something moves.
void HolderTick(Clock::time_point now, bool isHost) {
    TermState* t = FindTerm(g_shown);
    const bool holds = LocalHoldsTerm(t);
    void* term = SC::LocalTerminal();
    if (!holds) {
        g_wasHolding = false;
        return;
    }
    if (!term) return;
    if (!g_wasHolding || g_holdTerm != g_shown) {
        g_wasHolding = true;
        g_holdTerm = g_shown;
        ++g_myStamp;
        g_inSeq = 0;
        g_lastInputSent.clear();
        ReadHolderInput(term, g_lastInputSent);
        g_ctxSentValid = false;  // the prompt is sent once as the claim starts: the screen shows where the holder stands
        g_inputDirty = true;
        g_nextInput = now;
    }
    if (!sg::NameWatchLive(kRowTextChanged, kTagChanged) && now >= g_nextPoll) {
        if (!g_saidPoll) {
            g_saidPoll = true;
            UE_LOGW("sat_console: the input box's changed-text handler is not watched -- the input is polled "
                    "while this machine holds a terminal");
        }
        g_nextPoll = now + kPollEvery;
        g_inputDirty = true;
    }
    std::wstring name;
    int32_t dish = V::kNoDish;
    const bool haveCtx = ReadContext(term, name, dish);
    const bool ctxMoved = haveCtx && (!g_ctxSentValid || dish != g_lastDishSent || name != g_lastNameSent);
    if ((!g_inputDirty && !ctxMoved) || now < g_nextInput) return;
    std::wstring text;
    if (!ReadHolderInput(term, text)) return;
    g_inputDirty = false;
    if (text == g_lastInputSent && !ctxMoved) return;
    g_nextInput = now + kInputEvery;
    g_lastInputSent = text;
    if (haveCtx) {
        g_lastNameSent = name;
        g_lastDishSent = dish;
        g_ctxSentValid = true;
    }
    const uint32_t seq = ++g_inSeq;
    ++g_counts.inputsSent;
    if (isHost) {
        if (void* canon = Canon(*t, true)) {
            std::wstring cur;
            if (!ReadHolderInput(canon, cur) || cur != text) {
                Applying guard;
                SC::WriteInput(canon, text);
            }
        }
        if (t->pubValid) t->pub.input = text;
        V::InputRelay r;
        r.term = g_shown;
        r.holder = 0;
        r.gen = t->gen;
        r.seq = seq;
        r.text = text;
        ForEachClient([&](uint8_t to) {
            std::vector<uint8_t> b;
            V::EncodeInputRelay(b, r);
            EnqueueLatest(to, std::move(b));
        });
    } else {
        V::Input in;
        in.term = g_shown;
        in.stamp = g_myStamp;
        in.seq = seq;
        in.dish = dish;
        in.name = name;
        in.text = text;
        std::vector<uint8_t> b;
        V::EncodeInput(b, in);
        EnqueueLatest(0, std::move(b));
    }
    if (Trace())
        UE_LOGI("sat_console: %s input out at terminal %08X: stamp %u seq %u, %zu chars, prompt '%ls' dish %d",
                isHost ? "HOST" : "CLIENT", g_shown, g_myStamp, seq, text.size(), Printable(name).c_str(), dish);
}

// ---- the client ----

// A line whose command rests on the shared world goes to the host, with the terminal it was typed at and the context
// the console shows there: the dish (or ROOT) and the name. A busy console answers the line itself, as it would
// alone; so does a line the terminal owns. The body is refused, which leaves its own tail unrun, so the input is
// consumed here as that tail does (ui_console.enterCommand: `command = ""`, the text box emptied); the echo and every
// answer come back from the host, on that terminal's screen. On the host a line runs where it was typed, at the
// terminal the console shows.
sg::Verdict OnEnterPre(const sg::Call& c) {
    g_pendingClear = false;
    if (!InSession()) return sg::Verdict::Run;
    void* term = c.object;
    if (term != SC::LocalTerminal()) {
        if (!IsHost()) Note(Reason::LocalNotTerminal);
        return sg::Verdict::Run;
    }
    bool busy = false;
    std::wstring line;
    // What this body prints belongs to the terminal shown, unless the console's own command still runs (its
    // continuation keeps the terminal it started at; a switch waits for it).
    if (!g_localHolds) g_localCmdTerm = g_shown;
    if (!SC::ReadProcessing(term, busy) || !SC::ReadEnterCommandLine(c.locals, line)) {
        Note(Reason::LocalUnreadable);
        return sg::Verdict::Run;
    }
    if (busy) {
        if (!IsHost()) Note(Reason::LocalBusy, line);
        return sg::Verdict::Run;
    }
    if (IsHost()) {
        g_pendingClear = T::ClearsScreen(line);  // the canonical screen is reset once the console's own clr has run
        return sg::Verdict::Run;
    }
    if (!g_shown) {
        Note(Reason::LocalNotTerminal, line);
        return sg::Verdict::Run;
    }
    if (T::Classify(line) != T::Runs::OnHost) {
        Note(Reason::LocalOwnCommand, line);
        g_pendingClear = T::ClearsScreen(line);  // run here; its reset is sent on, once it has run
        return sg::Verdict::Run;
    }
    std::wstring name;
    if (!SC::ReadName(term, name)) {
        Note(Reason::LocalUnreadable, line);
        return sg::Verdict::Run;
    }
    if (name.size() > V::kMaxNameChars) name.resize(V::kMaxNameChars);
    if (line.size() > V::kMaxInputChars) line.resize(V::kMaxInputChars);
    V::Line out;
    out.term = g_shown;
    out.dish = DishIndexOf(term);
    out.name = name;
    out.line = line;
    std::vector<uint8_t> b;
    V::EncodeLine(b, out);
    Enqueue(0, std::move(b));
    SC::ConsumeInput(term);
    ++g_counts.linesSent;
    UE_LOGI("sat_console: CLIENT '%ls' runs on the host at terminal %08X (dish %d)", line.c_str(), g_shown, out.dish);
    return sg::Verdict::Cancel;
}

// After the console ran a body of its own. While the terminal it shows is busy elsewhere the flag is the mirror's: a
// line the console ran itself may have cleared it, and it is set again, the line's hold over. Else the flag is the
// line's own.
void AfterLocalBody(void* term) {
    bool busy = false;
    if (!SC::ReadProcessing(term, busy)) return;
    const TermState* t = FindTerm(g_shown);
    if (t && t->mirrorBusy) {
        if (!busy) {
            g_localHolds = false;
            SC::WriteProcessing(term, true);
        }
    } else {
        g_localHolds = busy;
    }
}

void SendPresent(TermId term, V::PresentKind kind, uint8_t type, const std::wstring& text) {
    if (!term) return;
    V::Present p;
    p.term = term;
    p.seq = ++g_presSeq;
    p.kind = kind;
    p.type = type > V::kMaxBrackets ? V::kMaxBrackets : type;
    p.text = text.size() > V::kMaxPrintChars ? text.substr(0, V::kMaxPrintChars) : text;
    std::vector<uint8_t> b;
    V::EncodePresent(b, p);
    Enqueue(0, std::move(b));
    ++g_counts.presentsSent;
}

// A screen kept back is counted once however many ticks it waits.
void NoteHeld(TermState& t) {
    if (t.heldCounted) return;
    t.heldCounted = true;
    ++g_counts.screensHeld;
    Note(Reason::ScreenHeld);
}

// The newest screen of the terminal the console shows, put on it. The typist (the claim's holder) keeps its own
// input and busy level, which are the source of the others'; it shows the history. None of it runs a command or
// prints: the history is written to the slot and repainted by the terminal's own fixLog, and the handlers our own
// writes wake are held off by `g_applying`.
void TryApplyPending(Clock::time_point now) {
    TermState* t = FindTerm(g_shown);
    if (!t || !t->pending) return;
    void* term = SC::LocalTerminal();
    const bool holding = LocalHoldsTerm(t);
    if (!term) {
        NoteHeld(*t);
        return;
    }
    // The typist's own printed lines are on this console and on their way to the host: a screen the host made
    // before it merged them would drop them for a moment, so it waits for the host's word, or a grace.
    if (holding && t->last.ackPresent < g_presSeq && now - t->pendingSince < kHeldGrace) {
        NoteHeld(*t);
        return;
    }
    if (!ApplyScreen(term, t->last, holding, *t)) return;  // the slot would not take it: kept, tried again
    t->pending = false;
    t->heldCounted = false;
    ++g_counts.screensApplied;
    if (Trace())
        UE_LOGI("sat_console: CLIENT terminal %08X screen rev %u applied (%zu chars, prompt '%ls' dish %d, holder %u "
                "gen %u%s)", t->id, t->last.rev, t->last.log.size(), Printable(t->last.name).c_str(), t->last.dish,
                static_cast<unsigned>(t->last.holder), t->last.gen, holding ? ", typist: history only" : "");
}

// A terminal's screen from the host: kept as that terminal's newest whether or not a panel here carries its id yet
// (one that streams in later shows it), and shown at once if the console shows that terminal.
void ClientOnScreen(const V::Screen& s) {
    TermState* t = GetTerm(s.term);
    if (!t) return;
    if (s.gen < t->lastGen) return;  // an older generation of the claim than one seen
    t->lastGen = s.gen;
    g_presAcked = std::max(g_presAcked, s.ackPresent);
    if (Trace())
        UE_LOGI("sat_console: CLIENT terminal %08X screen rev %u received (%zu chars, holder %u gen %u, acked present "
                "%u input %u)", s.term, s.rev, s.log.size(), static_cast<unsigned>(s.holder), s.gen, s.ackPresent,
                s.ackInput);
    if (!t->pending) t->pendingSince = Clock::now();
    t->heldCounted = false;
    t->last = s;
    t->haveLast = true;
    t->pending = true;
    if (s.term == g_shown) TryApplyPending(Clock::now());
}

void ClientOnRelay(const V::InputRelay& r) {
    TermState* t = GetTerm(r.term);
    if (!t) return;
    if (r.gen < t->lastGen) return;
    t->lastGen = r.gen;
    const std::wstring text = V::SanitizeInput(r.text);
    if (t->haveLast) t->last.input = text;
    if (r.term != g_shown || LocalHoldsTerm(t)) return;  // this machine's own typing is the source
    void* term = SC::LocalTerminal();
    if (!term) return;  // the next screen carries the text
    std::wstring cur;
    if (ReadHolderInput(term, cur) && cur == text) return;
    Applying guard;
    SC::WriteInput(term, text);
    if (Trace())
        UE_LOGI("sat_console: CLIENT input from holder %u at terminal %08X applied: seq %u, %zu chars",
                static_cast<unsigned>(r.holder), r.term, r.seq, text.size());
}

void ClientOnBlob(const std::vector<uint8_t>& blob) {
    uint8_t op = 0;
    uint32_t id = 0;
    if (!V::PeekHeader(blob, op, id)) {
        Note(Reason::BadPacket);
        return;
    }
    if (op == V::kOpBusy) {
        V::Busy v;
        if (!V::DecodeBusy(blob, v)) {
            Note(Reason::BadPacket);
            return;
        }
        TermState* t = GetTerm(v.term);
        if (!t) return;
        t->mirrorBusy = v.busy;
        if (v.term != g_shown) return;
        void* term = SC::LocalTerminal();
        if (term && (t->mirrorBusy || !g_localHolds)) SC::WriteProcessing(term, t->mirrorBusy);
        return;
    }
    if (op == V::kOpScreen) {
        V::Screen s;
        if (!V::DecodeScreen(blob, s)) {
            Note(Reason::BadPacket);
            return;
        }
        ClientOnScreen(s);
        return;
    }
    if (op == V::kOpInputRelay) {
        V::InputRelay r;
        if (!V::DecodeInputRelay(blob, r)) {
            Note(Reason::BadPacket);
            return;
        }
        ClientOnRelay(r);
        return;
    }
    Note(Reason::WrongDirection);
    UE_LOGW("sat_console: op %u reached a client -- dropped", static_cast<unsigned>(op));
}

// ---- the watches ----

// The terminal a print of the host's console outside any command belongs to: a panel's own alert (a server down at
// that panel) to that panel's terminal, the world's alerts to the desk's root terminal.
TermId AlertTerminal(void* callerObject) {
    if (SC::IsPanel(callerObject)) return SC::PanelId(callerObject);
    if (TermId root = SC::PanelId(SC::RootPanel())) return root;
    return g_shown;
}

sg::Verdict OnWritePre(const sg::Call& c) {
    if (SessionIf(coop::net::Role::Host)) {
        if (Executor* e = ExecByObject(c.object)) {
            std::wstring text;
            uint8_t type = 0;
            if (!SC::ReadWriteToLogArgs(c.locals, text, type)) return sg::Verdict::Run;
            // What an executor prints is on its terminal's screen, whether or not the typist is still there.
            if (MergeToScreen(e->term, text, type)) ++g_counts.linesReturned;
            return sg::Verdict::Run;
        }
        if (g_applying || c.object != SC::LocalTerminal()) return sg::Verdict::Run;  // a canonical screen: its post marks it
        std::wstring text;
        uint8_t type = 0;
        if (!SC::ReadWriteToLogArgs(c.locals, text, type)) return sg::Verdict::Run;
        // The host's console prints a command it runs itself onto the terminal the command started at, and an alert
        // onto the terminal it belongs to; it shows only what is its shown terminal's.
        const bool own = SC::InTerminalBody(c.object);
        const TermId to = own ? (g_localCmdTerm ? g_localCmdTerm : g_shown) : AlertTerminal(c.callerObject);
        if (to) MergeToScreen(to, text, type);
        return (to == g_shown || !to) ? sg::Verdict::Run : sg::Verdict::Cancel;
    }
    if (SessionIf(coop::net::Role::Client)) {
        if (c.object != SC::LocalTerminal() || g_applying) return sg::Verdict::Run;
        // A print inside a command this console runs is that command's, and goes to the host to be merged into the
        // terminal it started at. One outside any is the world's own alert, which the host's console gets too and
        // shows back on its terminal: a second copy here would be the only line the others lack.
        if (!SC::InTerminalBody(c.object)) {
            Note(Reason::AlertSuppressed);
            return sg::Verdict::Cancel;
        }
        std::wstring text;
        uint8_t type = 0;
        if (SC::ReadWriteToLogArgs(c.locals, text, type))
            SendPresent(g_localCmdTerm ? g_localCmdTerm : g_shown, V::PresentKind::Write, type, text);
    }
    return sg::Verdict::Run;
}

void OnWritePost(const sg::Call& c) {
    if (!SessionIf(coop::net::Role::Host)) return;
    if (TermState* t = ByCanon(c.object)) t->dirty = true;
}

void OnBodyPost(const sg::Call& c) {
    if (SessionIf(coop::net::Role::Host)) {
        if (Executor* e = ExecByObject(c.object)) {
            HostAfterBody(*e);
            return;
        }
        if (c.object != SC::LocalTerminal()) return;
        AfterLocalBody(c.object);
        if (TermState* t = FindTerm(g_localCmdTerm ? g_localCmdTerm : g_shown)) {
            t->dirty = true;  // the console's own command moved its terminal's busy level, or printed there
            if (c.tag == kTagEnter && g_pendingClear) {
                g_pendingClear = false;
                Owed o;
                o.clear = true;
                SubmitOwed(*t, std::move(o));
            }
        }
        if (c.tag == kTagEnter) {
            g_inputDirty = true;  // a submit empties the input: sent at once
            g_nextInput = Clock::time_point{};
        }
    } else if (SessionIf(coop::net::Role::Client)) {
        if (c.object != SC::LocalTerminal()) return;
        AfterLocalBody(c.object);
        if (c.tag == kTagEnter) {
            if (g_pendingClear) {
                g_pendingClear = false;
                SendPresent(g_localCmdTerm ? g_localCmdTerm : g_shown, V::PresentKind::Clear, 0, std::wstring());
            }
            g_inputDirty = true;  // a submit empties the input: sent at once
            g_nextInput = Clock::time_point{};
        }
    }
}

// The input box's text moved: a person typed, or our own write did.
sg::Verdict OnTextChangedPre(const sg::Call& c) {
    if (g_applying && c.object == SC::LocalTerminal()) return sg::Verdict::Cancel;  // a replica's text is not a j or k to scroll by
    return sg::Verdict::Run;
}

void OnInputPost(const sg::Call& c) {
    if (g_applying || !InSession() || c.object != SC::LocalTerminal()) return;
    g_inputDirty = true;
}

// A panel's begin-overlap: remembered while its body runs, so the con() it makes knows who walked in.
sg::Verdict OnOverlapPre(const sg::Call& c) {
    g_overlap.active = true;
    g_overlap.panel = c.object;
    g_overlap.actor = InSession() ? SC::ReadOverlapActor(c.locals) : nullptr;
    return sg::Verdict::Run;
}

void OnOverlapPost(const sg::Call&) { g_overlap = OverlapNow{}; }

// A panel's con(), in a session: refused always -- it would init the one console every panel shows. In its place the
// console shows that panel's terminal when this machine's own player used it, committed at it, or walked into it, and
// stays where it is when another player walked in or this one is inside a terminal already.
sg::Verdict OnConPre(const sg::Call& c) {
    if (!InSession() || !SC::IsPanel(c.object)) return sg::Verdict::Run;
    void* w = SC::LocalTerminal();
    const TermId id = SC::PanelId(c.object);
    const bool overlap = g_overlap.active && g_overlap.panel == c.object;
    const char* source = overlap ? "this player walked in" : (w && c.callerObject == w ? "commit" : "use");
    const char* refused = nullptr;
    if (overlap) {
        void* local = coop::players::Registry::Get().Local();
        if (!local || g_overlap.actor != local) refused = "another player walked into the panel";
        else if (LocalHoldsTerm(FindTerm(g_shown)) && id != g_shown) refused = "this machine's player is inside another terminal";
    }
    std::wstring prompt;
    if (w) SC::ReadName(w, prompt);
    if (refused) {
        Note(Reason::BlockedCon, SC::KeyForId(id));
        SayDecision("panel '%ls' (terminal %08X) con() refused: %s (actor %p) -- console %p keeps terminal %08X '%ls'",
                    SC::PanelName(c.object).c_str(), id, refused, g_overlap.actor, w, g_shown, Printable(prompt).c_str());
        return sg::Verdict::Cancel;
    }
    if (id == g_shown && g_wantShown == 0) return sg::Verdict::Cancel;  // it shows that terminal already
    SayDecision("panel '%ls' (terminal %08X) con() by %s: console %p from terminal %08X '%ls'",
                SC::PanelName(c.object).c_str(), id, source, w, g_shown, Printable(prompt).c_str());
    ShowTerminal(id, source);
    return sg::Verdict::Cancel;
}

// The backstop: any native init of this machine's console in a session would re-point the screen without the lane;
// refused and said, with what it would have set.
sg::Verdict OnInitPre(const sg::Call& c) {
    if (!InSession() || g_applying) return sg::Verdict::Run;
    void* w = SC::LocalTerminal();
    if (!w || c.object != w) return sg::Verdict::Run;
    std::wstring name, oldName;
    void* dish = nullptr;
    SC::ReadInitArgs(c.locals, name, dish);
    SC::ReadName(w, oldName);
    Note(Reason::BlockedInit, name);
    SayDecision("a native init of console %p refused (from %p): prompt '%ls' -> '%ls', dish %p; it shows terminal %08X "
                "(used panel %p)", w, c.callerObject, Printable(oldName).c_str(), Printable(name).c_str(), dish, g_shown,
                SC::ReadUsed(w));
    return sg::Verdict::Cancel;
}

// The core three stand or fall together: a line refused on a client whose answers no watch would put on the
// screen would vanish. The panel guard (con, its overlap, init) stands apart: without it the console is re-pointed
// natively, as before, and that is said. The input watches are extra: with the changed-text handler unwatched the
// holder's input is polled instead.
void EnsureWatched() {
    if (g_watched) return;
    g_watched = true;
    const bool enter = sg::WatchName(kRowEnterCommand, kTagEnter, OnEnterPre, OnBodyPost);
    const bool graph = sg::WatchName(kRowUbergraph, kTagGraph, nullptr, OnBodyPost);
    const bool write = sg::WatchName(kRowWriteToLog, kTagWrite, OnWritePre, OnWritePost);
    if (!(enter && graph && write)) {
        if (enter) sg::UnwatchName(kRowEnterCommand, kTagEnter, OnEnterPre, OnBodyPost);
        if (graph) sg::UnwatchName(kRowUbergraph, kTagGraph, nullptr, OnBodyPost);
        if (write) sg::UnwatchName(kRowWriteToLog, kTagWrite, OnWritePre, OnWritePost);
        UE_LOGE("sat_console: a terminal watch did not register -- none kept, the SAT console's lines run where typed");
        return;
    }
    const bool con = sg::WatchName(kRowCon, kTagCon, OnConPre, nullptr);
    const bool overlap = sg::WatchName(kRowOverlap, kTagOverlap, OnOverlapPre, OnOverlapPost);
    const bool init = sg::WatchName(kRowInit, kTagInit, OnInitPre, nullptr);
    if (!(con && overlap && init))
        UE_LOGW("sat_console: a panel guard watch did not register (con=%d overlap=%d init=%d) -- a panel may re-point "
                "this machine's console at another terminal", con ? 1 : 0, overlap ? 1 : 0, init ? 1 : 0);
    const bool changed = sg::WatchName(kRowTextChanged, kTagChanged, OnTextChangedPre, OnInputPost);
    const bool type = sg::WatchName(kRowTypeSymbol, kTagType, nullptr, OnInputPost);
    const bool key = sg::WatchName(kRowKeyDown, kTagKey, nullptr, OnInputPost);
    if (!(changed && type && key))
        UE_LOGW("sat_console: an input watch did not register (changed=%d typeSymbol=%d keyDown=%d) -- the input is "
                "polled while a machine holds a terminal", changed ? 1 : 0, type ? 1 : 0, key ? 1 : 0);
}

void ClearTerms() {
    for (auto& t : g_terms) DropCanon(*t);
    g_terms.clear();
    g_saidTermsFull = false;
}

// The screen state belongs to its world: with a new one, every terminal's state is over (the panels and their
// canonical screens are the old world's), and the console picks its terminal again.
void OnWorldChanged() {
    ClearTerms();
    g_shown = 0;
    g_wantShown = 0;
    g_nextShowTry = Clock::time_point{};
    g_seeded = false;
    g_wasHolding = false;
    g_ctxSentValid = false;
    g_pendingClear = false;
    g_localHolds = false;
    g_localCmdTerm = 0;
    g_overlap = OverlapNow{};
    for (bool& b : g_owedSnapshot) b = false;
    if (g_holding) SC::HoldOffscreenTick(false);  // the old world's component is no more: nothing of its to put back
    g_holding = false;
    g_holdUntil = Clock::time_point{};
    UE_LOGI("sat_console: the world changed -- every terminal's screen is read again from this world");
}

// The desk's widget component ticks offscreen while a remote typist holds a terminal, a line waits or an executor
// runs a command, and for kTickLinger after.
void TickOffscreen(Clock::time_point now) {
    bool want = false;
    for (auto& t : g_terms) {
        if ((t->holder != V::kNoHolder && t->holder != 0) || !t->waiting.empty() || !t->owed.empty() ||
            t->heldInput.valid) {
            want = true;
            break;
        }
    }
    if (!want)
        for (const Executor& e : g_execs) {
            bool busy = false;
            if (SC::ReadProcessing(e.pin.Raw(), busy) && busy) {
                want = true;
                break;
            }
        }
    if (want) g_holdUntil = now + kTickLinger;
    if (now < g_holdUntil) {
        g_holding = true;
        SC::HoldOffscreenTick(true);
    } else if (g_holding) {
        g_holding = false;
        SC::HoldOffscreenTick(false);
    }
}

// HOST: every placed panel is a terminal whose holder is tracked, whether or not anything was printed there yet.
void RefreshPanels(Clock::time_point now) {
    if (now < g_nextPanels) return;
    g_nextPanels = now + kPanelRefresh;
    std::vector<void*> panels;
    SC::EnumeratePanels(panels);
    for (void* p : panels) GetTerm(SC::PanelId(p));
}

// This machine's console: its first terminal, a switch that waited, and the holder's typing.
void LocalTick(Clock::time_point now, bool isHost) {
    if (!g_shown) InitialShow(now);
    else if (SC::LocalTerminal()) {
        if (g_wantShown && !g_localHolds && now >= g_nextShowTry) {
            g_nextShowTry = now + kShowRetry;
            ShowTerminal(g_wantShown, g_wantSource);
        }
    }
    HolderTick(now, isHost);
}

void HostTick(Clock::time_point now) {
    RefreshPanels(now);
    // By index: a body these calls wake may give a new terminal state (the vector grows; the states do not move).
    for (size_t i = 0; i < g_terms.size(); ++i) {
        TermState& t = *g_terms[i];
        const uint8_t holder = DO::HolderOf(t.key.c_str());
        if (holder != t.holder) {
            t.holder = holder;
            ++t.gen;
            t.dirty = true;
            UE_LOGI("sat_console: HOST terminal %08X is %s (generation %u)", t.id,
                    holder == V::kNoHolder ? "free"
                                           : ("held by slot " + std::to_string(static_cast<unsigned>(holder))).c_str(),
                    t.gen);
        }
    }
    for (int i = 1; i < kMaxPeers; ++i)
        if (g_owedSnapshot[i]) SendSnapshot(static_cast<uint8_t>(i));
    for (size_t i = 0; i < g_terms.size(); ++i) {
        FlushOwed(*g_terms[i]);
        TickWaiting(*g_terms[i], now);
        TickHeldInput(*g_terms[i]);
    }
    LocalTick(now, true);
    TickOffscreen(now);
    for (size_t i = 0; i < g_terms.size(); ++i) {
        TermState& t = *g_terms[i];
        if (!t.dirty || now < t.nextScreen) continue;
        t.dirty = false;  // cleared before the publish, so a body that runs during it keeps it dirty
        t.nextScreen = now + kScreenEvery;
        if (!HostPublish(t, false)) {
            t.dirty = true;  // not published: still owed, tried again
            t.nextScreen = now + kPublishRetry;
        }
    }
}

void ClientTick(Clock::time_point now) {
    LocalTick(now, false);
    TryApplyPending(now);
}

}  // namespace

void Install(coop::net::Session* s) {
    g_session.store(s, std::memory_order_release);
    EnsureWatched();
}

void Tick() {
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    SC::EnsureResolved();
    const auto now = Clock::now();
    for (int i = 0; i < kMaxPeers; ++i)
        if (!g_out[i].q.empty() && now >= g_out[i].nextTry) Flush(static_cast<uint8_t>(i));
    // An executor holds its world's desk, dish and panel: one made in a world that is known and no longer the current
    // one goes. An unknown world keeps it.
    if (void* world = WI::CurrentWorld()) {
        if (world != g_world) {
            if (g_world) OnWorldChanged();
            g_world = world;
        }
        for (size_t i = 0; i < g_execs.size();) {
            if (!g_execs[i].world || g_execs[i].world == world) {
                ++i;
                continue;
            }
            UE_LOGI("sat_console: HOST an executor discarded with its world");
            DiscardExecAt(i);
        }
    }
    if (s->role() == coop::net::Role::Host) HostTick(now);
    else if (s->role() == coop::net::Role::Client) ClientTick(now);
    if (!g_asm.Idle() && now >= g_nextSweep) {
        g_nextSweep = now + std::chrono::seconds(1);
        g_asm.Sweep(now, std::chrono::seconds(10));
    }
}

void OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!GT::IsGameThread()) {
        if (!g_saidOffThread) {
            g_saidOffThread = true;
            UE_LOGE("sat_console: a chunk arrived off the game thread -- dropped");
        }
        return;
    }
    if (senderSlot >= kMaxPeers) return;
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(p, senderSlot, blob)) return;
    if (SessionIf(coop::net::Role::Host)) {
        if (senderSlot != 0) HostOnBlob(senderSlot, blob);
    } else if (SessionIf(coop::net::Role::Client)) {
        if (senderSlot == 0) ClientOnBlob(blob);
    }
}

void OnPeerWorldReady(int slot) {
    if (!SessionIf(coop::net::Role::Host) || slot < 1 || slot >= kMaxPeers) return;
    const std::string& guid = coop::player_handshake::GuidForSlot(slot);
    if (!guid.empty())
        for (Executor& e : g_execs)
            if (e.guid == guid && e.slot != slot) Rebind(e, static_cast<uint8_t>(slot));
    // A peer new to the session, or back, is shown every terminal's screen, even when nothing has moved since.
    SendSnapshot(static_cast<uint8_t>(slot));
}

void OnPeerGone(uint8_t slot) {
    if (slot >= kMaxPeers) return;
    g_asm.ClearSlot(slot);
    g_out[slot] = Outbox{};
    g_saidRefused[slot] = g_saidMalformed[slot] = g_saidNoTerminal[slot] = g_saidNoContext[slot] = false;
    g_slotStamp[slot] = g_slotInSeq[slot] = g_slotPresSeq[slot] = 0;
    g_queuedPresSeq[slot] = 0;
    g_owedSnapshot[slot] = false;
    // Its lines that never ran are not run for a typist that is gone (a line is run for its sender, once); what it
    // printed is still owed to the screen, but no longer acknowledged to a slot that may have another occupant.
    for (auto& tp : g_terms) {
        TermState& t = *tp;
        for (auto it = t.waiting.begin(); it != t.waiting.end();) it = (it->slot == slot) ? t.waiting.erase(it) : it + 1;
        for (Owed& o : t.owed)
            if (o.ackSlot == slot) o.ackSlot = -1;
        if (t.heldInput.valid && t.heldInput.slot == slot) t.heldInput.valid = false;
    }
    for (Executor& e : g_execs) {
        if (e.slot != slot) continue;
        e.slot = -1;  // its command, if any, runs to its end; the executor waits for its typist
        UE_LOGI("sat_console: HOST slot %u left -- its executor for terminal %08X kept", static_cast<unsigned>(slot), e.term);
    }
}

void OnDisconnect() {
    // A busy flag this console holds only as the host's mirror goes with the session.
    const TermState* shown = FindTerm(g_shown);
    if (shown && shown->mirrorBusy && !g_localHolds)
        if (void* term = SC::LocalTerminal()) SC::WriteProcessing(term, false);
    g_localHolds = false;
    while (!g_execs.empty()) DiscardExecAt(g_execs.size() - 1);
    g_budget.clear();
    ClearTerms();
    for (auto& o : g_out) o = Outbox{};
    for (bool& b : g_saidRefused) b = false;
    for (bool& b : g_saidMalformed) b = false;
    for (bool& b : g_saidNoTerminal) b = false;
    for (bool& b : g_saidNoContext) b = false;
    for (bool& b : g_owedSnapshot) b = false;
    for (uint32_t& v : g_slotStamp) v = 0;
    for (uint32_t& v : g_slotInSeq) v = 0;
    for (uint32_t& v : g_slotPresSeq) v = 0;
    for (uint32_t& v : g_queuedPresSeq) v = 0;
    if (g_holding) SC::HoldOffscreenTick(false);
    g_holding = false;
    g_holdUntil = Clock::time_point{};
    for (CR::Site site : {CR::Site::SatLine, CR::Site::SatMerge, CR::Site::SatInput, CR::Site::SatPublish})
        CR::ResetSite(site);
    g_asm.Clear();
    if (g_counts.linesSent || g_counts.linesRun)
        UE_LOGI("sat_console: session end -- %llu line(s) sent to the host, %llu run for clients",
                static_cast<unsigned long long>(g_counts.linesSent),
                static_cast<unsigned long long>(g_counts.linesRun));
    if (g_counts.screensSent || g_counts.screensApplied || g_counts.inputsSent || g_counts.inputsTaken)
        UE_LOGI("sat_console: session end -- screens: %llu sent, %llu applied, %llu held; input: %llu sent, %llu taken, "
                "%llu refused; prints: %llu sent, %llu merged",
                static_cast<unsigned long long>(g_counts.screensSent),
                static_cast<unsigned long long>(g_counts.screensApplied),
                static_cast<unsigned long long>(g_counts.screensHeld),
                static_cast<unsigned long long>(g_counts.inputsSent),
                static_cast<unsigned long long>(g_counts.inputsTaken),
                static_cast<unsigned long long>(g_counts.inputsStale),
                static_cast<unsigned long long>(g_counts.presentsSent),
                static_cast<unsigned long long>(g_counts.presentsMerged));
    for (size_t i = 0; i < kReasons; ++i)
        if (g_reason[i]) UE_LOGI("sat_console: session end -- %llu x %s", static_cast<unsigned long long>(g_reason[i]), kReasonText[i]);
    for (size_t i = 0; i < kReasons; ++i) {
        g_reason[i] = 0;
        g_reasonSaid[i] = false;
    }
    g_counts = Counts{};
    g_shown = 0;
    g_wantShown = 0;
    g_nextShowTry = Clock::time_point{};
    g_seeded = false;
    g_localCmdTerm = 0;
    g_overlap = OverlapNow{};
    g_diagSaid = 0;
    g_nextPanels = Clock::time_point{};
    g_wasHolding = false;
    g_holdTerm = 0;
    g_inputDirty = false;
    g_myStamp = 0;
    g_inSeq = 0;
    g_ctxSentValid = false;
    g_lastInputSent.clear();
    g_presSeq = g_presAcked = 0;
    g_pendingClear = false;
    g_world = nullptr;
    g_saidPoll = false;
}

Counts LaneCounts() {
    return g_counts;
}

size_t KeptTerminals(void** out, size_t cap) {
    size_t n = 0;
    for (const Executor& e : g_execs)
        if (n < cap) out[n++] = e.pin.Raw();
    return n;
}

uint32_t ShownTerminal() { return g_shown; }

bool ShowTerminalForDish(int32_t dish) {
    if (!InSession() || dish < 0) return false;
    void* d = ue_wrap::dish::DishByIndex(dish);
    if (!d) return false;
    std::vector<void*> panels;
    SC::EnumeratePanels(panels);
    for (void* p : panels) {
        const SC::TerminalBinding b = SC::TerminalForPanel(p);
        if (b.ready && b.dish == d) return ShowTerminal(b.id, "drill") && g_shown == b.id;
    }
    return false;
}

}  // namespace coop::sat_console_sync

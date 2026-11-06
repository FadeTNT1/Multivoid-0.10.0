// coop/interactables/sat_console_sync.cpp -- see coop/interactables/sat_console_sync.h.

#include "coop/interactables/sat_console_sync.h"

#include "coop/config/config.h"
#include "coop/interactables/computer_readiness.h"
#include "coop/interactables/device_occupancy.h"  // LocalHolds, HolderOf: who has the SAT console
#include "coop/interactables/sat_console_table.h"
#include "coop/interactables/sat_console_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
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
#include <cwchar>
#include <deque>
#include <string>
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

constexpr int    kLinesPerWindow = 4;     // a person types slower: lines a typist may run per window
constexpr auto   kWindow         = std::chrono::seconds(1);
constexpr auto   kSpawnEvery     = std::chrono::seconds(2);  // a typist's spawn-on-every-call commands
constexpr size_t kMaxTerminals   = 16;    // kept terminals, by identity, while the session lasts
constexpr size_t kOutboxCap      = 512;   // blobs owed to one slot while its sends are refused
constexpr auto   kResendEvery    = std::chrono::milliseconds(250);  // a refused slot is tried this often
constexpr auto   kScreenEvery    = std::chrono::milliseconds(100);  // the screen goes out at most ten times a second
constexpr auto   kInputEvery     = std::chrono::milliseconds(50);   // the holder's input goes out at most twenty times a second
constexpr auto   kPollEvery      = std::chrono::milliseconds(100);  // the input is read this often when no watch tells of a change
constexpr auto   kHeldGrace      = std::chrono::seconds(2);         // a screen kept back for the typist's own output waits this long
constexpr auto   kLineWaitMax    = std::chrono::seconds(10);        // a client's line kept for a terminal that is not up, then answered not ready
constexpr size_t kMaxWaitingPerSlot = 8;                            // lines one typist may have kept
constexpr size_t kOwedCap        = 256;                             // prints kept for the host's terminal while it is not up
constexpr int    kOwedAttempts   = 5;                               // a print that reached a live terminal and still failed this many times is dropped
constexpr auto   kPublishRetry   = std::chrono::milliseconds(500);  // a screen that could not be published is tried again this often
constexpr auto   kTickLinger     = std::chrono::seconds(2);         // the terminal keeps ticking offscreen this long after the last remote work
const wchar_t* const kSatKey   = L"sat";                            // the device claim of the SAT console
const wchar_t* const kBusyLine = L"<r>The terminal is busy</>";  // the game's answer to a line it cannot take
const wchar_t* const kErrLine  = L"<r>err</>";                   // the game's answer to a line it cannot run
const wchar_t* const kNotReadyLine = L"<r>The terminal is not ready</>";  // a line the host's terminal never came up for

UE_FACT_FN(kRowEnterCommand, "sat_console_sync", L"ui_console_C", L"enterCommand", Declared);
UE_FACT_FN(kRowUbergraph, "sat_console_sync", L"ui_console_C", L"ExecuteUbergraph_ui_console", Declared);
UE_FACT_FN(kRowWriteToLog, "sat_console_sync", L"ui_console_C", L"writeToLog", Declared);
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

// Why a line, a print or a packet went where it did and not where one would expect: a bounded set of
// counters, the first of each said once, the totals said when the session ends. [sat_console_trace] says each.
enum class Reason : uint8_t {
    LocalOwnCommand,    // a client's line ran at its own terminal: the command is the typist's own
    LocalBusy,          // ... its terminal was busy, and answers the line itself
    LocalUnreadable,    // ... its terminal's flag or name could not be read
    LocalNotTerminal,   // ... it was not typed at this machine's terminal
    AlertSuppressed,    // a client's terminal printed outside any command: the host's copy of it is the one shown
    NotHolder,          // a packet from a peer the claim table does not name as holding the console
    StaleInput,         // an input or present packet no newer than the last taken
    BadPacket,          // a packet that did not decode within its bounds
    WrongDirection,     // an op that goes the other way, or one retired
    NoScreenTerminal,   // the host has no terminal of its own to put the screen on
    ScreenHeld,         // a screen kept back, for the typist's own unmerged output or a terminal not up
    NoContext,          // a replica could not resolve the prompt's dish or init failed
    LineKept,           // a client's line waited, unrun, for the host's terminal to come up
    LineNotReady,       // ... and the terminal never did within the wait: the typist was told
    PrintKept,          // a print waited for the host's terminal to come up
    InputKept,          // the holder's input waited for the host's terminal to come up
    Count
};
constexpr size_t kReasons = static_cast<size_t>(Reason::Count);
const char* const kReasonText[kReasons] = {
    "a line ran at its typist's own terminal: the command is the typist's own",
    "a line ran at its typist's own terminal: the terminal was busy",
    "a line ran at its typist's own terminal: the terminal could not be read",
    "a line ran at its typist's own terminal: it was not typed at this machine's terminal",
    "a client's terminal printed outside a command: not shown, the host's copy is",
    "a packet came from a peer that does not hold the SAT console: dropped",
    "an input or output packet was no newer than the last taken: dropped",
    "a packet did not decode within its bounds: dropped",
    "an op arrived the wrong way round: dropped",
    "the host has no terminal of its own to show the screen on",
    "a screen was kept back (the typist's own output is not merged yet, or no terminal is up)",
    "the prompt's dish could not be resolved or its init failed: the prompt stays",
    "a client's line was kept, not run, until the host's terminal came up",
    "a client's line waited ten seconds for the host's terminal and was answered not ready, never run",
    "a print was kept until the host's terminal came up, and written in order then",
    "the holder's input was kept until the host's terminal came up",
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

// A blob that states the whole of something (a screen, an input line) replaces one of its op still owed: the
// older is never shown, so a slot whose sends are refused for a while owes one of each, not a backlog.
void EnqueueLatest(uint8_t slot, std::vector<uint8_t> blob) {
    Outbox& o = g_out[slot];
    const uint8_t op = blob.empty() ? 0xFF : blob[0];
    o.q.erase(std::remove_if(o.q.begin(), o.q.end(),
                             [op](const std::vector<uint8_t>& b) { return !b.empty() && b[0] == op; }),
              o.q.end());
    Enqueue(slot, std::move(blob));
}

// ---- state: the host's ----

// HOST: the terminal each typist runs on, keyed by its durable identity, not its slot: a typist that
// leaves mid-command leaves its terminal running that command, as the game's terminal runs on when its
// player walks away, and a typist that comes back finds it again. A terminal lasts while the session
// does and its world stays current; the last client's leave ends the session, and each terminal is
// then discarded, its command stopping where it stands.
struct Terminal {
    std::string guid;
    ue_wrap::GcPin pin;
    void*   world = nullptr;  // the world it was made in, when known
    int     slot = -1;        // its typist's slot while connected, -1 once it left
    bool    busySent = false; // the busy level its typist last got
    bool    haveContext = false;
    int32_t dish = V::kNoDish;
    std::wstring name;
    int     lines = 0;        // lines run in the current window
    Clock::time_point windowStart{};
    Clock::time_point nextSpawnOk{};
};
std::vector<Terminal> g_terms;
bool g_saidRefused[kMaxPeers] = {};
bool g_saidMalformed[kMaxPeers] = {};
bool g_saidNoTerminal[kMaxPeers] = {};
bool g_saidNoContext[kMaxPeers] = {};

// HOST: the shared screen's bookkeeping. The canonical screen is the host's own terminal; what is kept here is
// what was last published (to publish only a change), who holds the console and under which generation, and
// per client slot the newest claim count, input sequence and presentation sequence taken from it.
V::Screen g_pub;             // the last screen published, its input aside
bool      g_pubValid = false;
uint32_t  g_rev = 0;
uint32_t  g_gen = 0;
uint8_t   g_holder = V::kNoHolder;
bool      g_screenDirty = false;
Clock::time_point g_nextScreen{};
uint32_t  g_slotStamp[kMaxPeers] = {};
uint32_t  g_slotInSeq[kMaxPeers] = {};
uint32_t  g_slotPresSeq[kMaxPeers] = {};
bool      g_owedScreen[kMaxPeers] = {};  // a slot told its world is ready before the host had a terminal to read

// A client's line that arrived while the host's own terminal was not up, kept unrun and in order; it runs when the
// terminal is, once, and is answered not ready after kLineWaitMax. Only a line that has not run is ever kept: a line
// that ran (or was answered) is gone from here, so no command is run twice.
struct WaitingLine {
    uint8_t slot = 0;
    V::Line line;
    Clock::time_point since{};
};
std::deque<WaitingLine> g_waiting;

// A print, or a reset, owed to the host's own terminal: written in the order it was asked, once the terminal is up.
// An entry carries the present sequence it acknowledges, which is taken (acked) only as it is written.
struct Owed {
    bool         clear = false;
    std::wstring text;
    uint8_t      type = 0;
    int          ackSlot = -1;
    uint32_t     ackSeq = 0;
    int          attempts = 0;
};
std::deque<Owed> g_owed;
bool      g_saidOwedFull = false;
uint32_t  g_queuedPresSeq[kMaxPeers] = {};  // the newest present sequence taken into the owed queue, per slot

// The holder's newest input, kept while the host's terminal is not up: applied (and acked) once it is.
struct HeldInput {
    bool     valid = false;
    uint8_t  slot = 0;
    V::Input in;
};
HeldInput g_heldInput;

Clock::time_point g_holdUntil{};  // the desk's widget component ticks offscreen until this
bool      g_holding = false;

// ---- state: the holder's input, host or client ----

bool      g_wasHolding = false;
uint32_t  g_myStamp = 0;     // this machine's count of its own claims of the console
uint32_t  g_inSeq = 0;
bool      g_inputDirty = false;
Clock::time_point g_nextInput{};
Clock::time_point g_nextPoll{};
std::wstring g_lastInputSent;
std::wstring g_lastNameSent;
int32_t   g_lastDishSent = V::kNoDish;
bool      g_ctxSentValid = false;

// ---- state: a client's ----

// The host's busy level for this terminal, and whether a line this terminal ran itself holds the flag: the
// mirror sets it while the host's terminal is busy and clears only what it set.
bool g_mirrorBusy = false;
bool g_localHolds = false;
uint32_t g_presSeq = 0;     // the presentation sequence of the last print sent to the host
uint32_t g_presAcked = 0;   // the newest of them the host says it merged
bool g_pendingClear = false;  // the local line now running empties the screen
bool g_heldCounted = false;
bool g_havePending = false;   // the newest screen is kept: no terminal to show it on yet, or held back
V::Screen g_pending;
Clock::time_point g_pendingSince{};
uint32_t g_lastGen = 0;

// ---- the screen: reading ----

std::wstring ClampTail(const std::wstring& s, size_t n) {
    return s.size() > n ? s.substr(s.size() - n) : s;
}

int32_t DishIndexOf(void* term) {
    void* d = SC::ReadActiveDish(term);
    const int32_t i = d ? ue_wrap::dish::IndexOf(d) : V::kNoDish;
    return i < V::kNoDish ? V::kNoDish : i;
}

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

// ---- the host ----

Terminal* ByObject(void* obj) {
    if (!obj) return nullptr;
    for (Terminal& t : g_terms)
        if (t.pin.Raw() == obj) return &t;
    return nullptr;
}

Terminal* ByGuid(const std::string& guid) {
    for (Terminal& t : g_terms)
        if (t.guid == guid) return &t;
    return nullptr;
}

void SendBusy(uint8_t slot, bool busy) {
    std::vector<uint8_t> b;
    V::EncodeBusy(b, busy);
    Enqueue(slot, std::move(b));
}

// Every peer that has a connection and a world: where the screen goes.
template <class F>
void ForEachClient(F&& f) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    for (int slot = 1; slot < kMaxPeers; ++slot)
        if (s->HasPeerConn(slot) && s->IsSlotWorldReady(slot)) f(static_cast<uint8_t>(slot));
}

// The host's terminal as the canonical screen, read after the game's own append and trim.
bool HostBuildScreen(void* vis, V::Screen& out) {
    std::wstring log;
    if (!SC::ReadLog(vis, log) || !ReadContext(vis, out.name, out.dish)) return false;
    out.log = ClampTail(log, V::kMaxHistory);
    if (!ReadHolderInput(vis, out.input)) return false;
    bool busy = false;
    SC::ReadProcessing(vis, busy);
    // The holder's own terminal, which the host keeps for it, is busy while its command runs.
    if (!busy && g_holder != V::kNoHolder && g_holder != 0) {
        const std::string& guid = coop::player_handshake::GuidForSlot(g_holder);
        if (Terminal* t = guid.empty() ? nullptr : ByGuid(guid)) SC::ReadProcessing(t->pin.Raw(), busy);
    }
    out.busy = busy ? 1 : 0;
    out.gen = g_gen;
    out.holder = g_holder;
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

// Where the host's widget chain breaks, in the readiness trace's words: the actor, each link, the claim holder.
void ReportNoScreen(CR::Site site, const char* what) {
    const SC::ScreenDiag d = SC::DiagnoseLocal();
    CR::Report(site, d.status == SC::ScreenStatus::NoDesk ? CR::Why::MissingActor : CR::Why::MissingWidget,
               "%s: %s (desk %p, desk widget %p, terminal %p, desk widget component %p%s), claim holder %u", what,
               SC::StatusText(d.status), d.desk, d.atlas, d.terminal, d.widgetComponent,
               d.componentHoldsAtlas ? " holding the atlas" : "", static_cast<unsigned>(DO::HolderOf(kSatKey)));
}

// Publish the screen when it moved: the history, the prompt or the busy level (the input rides its own packets).
// False when it could not be read or published, so the caller keeps the screen dirty and tries again; true when it
// was sent or had not moved.
bool HostPublish(bool force) {
    void* vis = SC::LocalTerminal();
    if (!vis) {
        Note(Reason::NoScreenTerminal);
        ReportNoScreen(CR::Site::SatPublish, "the screen cannot be published");
        return false;
    }
    V::Screen cur;
    if (!HostBuildScreen(vis, cur)) {
        CR::Report(CR::Site::SatPublish, CR::Why::DispatchFailed,
                   "the host terminal %p's log, prompt or input could not be read", vis);
        return false;
    }
    const bool changed = force || !g_pubValid || cur.log != g_pub.log || cur.name != g_pub.name ||
                         cur.dish != g_pub.dish || cur.busy != g_pub.busy || cur.holder != g_pub.holder ||
                         cur.gen != g_pub.gen;
    if (!changed) return true;
    cur.rev = ++g_rev;
    g_pub = cur;
    g_pubValid = true;
    size_t peers = 0;
    ForEachClient([&](uint8_t slot) {
        SendScreenTo(slot, cur);
        ++peers;
    });
    if (Trace())
        UE_LOGI("sat_console: HOST screen rev %u to %zu peer(s): %zu chars, prompt '%ls' dish %d, busy %d, holder %u gen %u",
                cur.rev, peers, cur.log.size(), Printable(cur.name).c_str(), cur.dish, static_cast<int>(cur.busy),
                static_cast<unsigned>(cur.holder), cur.gen);
    return true;
}

// A late joiner, or a peer whose world just became ready: the whole screen, even if nothing moved.
void SendWholeScreen(uint8_t slot) {
    void* vis = SC::LocalTerminal();
    V::Screen cur;
    if (!vis || !HostBuildScreen(vis, cur)) {
        g_owedScreen[slot] = true;  // asked again from the tick, while the desk comes up
        Note(Reason::NoScreenTerminal);
        return;
    }
    g_owedScreen[slot] = false;
    cur.rev = g_rev;
    SendScreenTo(slot, cur);
    UE_LOGI("sat_console: HOST sent slot %u the whole screen (%zu chars, rev %u, holder %u)", static_cast<unsigned>(slot),
            cur.log.size(), cur.rev, static_cast<unsigned>(cur.holder));
}

// What a typist whose screen cannot be published is told: the last published screen with one line more, in the prompt
// the line named. Best effort -- the terminal that would hold the canonical text is not up -- and the lines the host
// owes its terminal are written in their order when it is, so the notice is in the history then. A client applies it
// like any screen, and the next real publish replaces it.
void SendNoticeScreen(uint8_t slot, const V::Line& in, const wchar_t* text) {
    V::Screen s = g_pubValid ? g_pub : V::Screen{};
    s.log = ClampTail(s.log + (s.log.empty() ? std::wstring() : std::wstring(L"\n")) + text, V::kMaxHistory);
    s.name = in.name;
    s.dish = in.dish;
    s.input.clear();
    s.busy = 0;
    s.holder = g_holder;
    s.gen = g_gen;
    s.rev = g_rev;
    SendScreenTo(slot, s);
}

// Write one owed entry to the host's terminal; false when the call did not run.
bool ApplyOwed(void* vis, const Owed& o) {
    if (o.clear) {
        Applying guard;
        return SC::WriteLog(vis, std::wstring());
    }
    return SC::CallWriteToLog(vis, o.text, o.type);
}

// An entry written: the screen moved, and the print it acknowledged is taken.
void FinishOwed(const Owed& o) {
    g_screenDirty = true;
    if (o.ackSlot >= 0 && o.ackSlot < kMaxPeers) {
        g_slotPresSeq[o.ackSlot] = std::max(g_slotPresSeq[o.ackSlot], o.ackSeq);
        ++g_counts.presentsMerged;
        if (Trace())
            UE_LOGI("sat_console: HOST merged slot %d's %s (seq %u)", o.ackSlot, o.clear ? "clr" : "print", o.ackSeq);
    }
    if (CR::TraceOn())
        CR::Report(CR::Site::SatMerge, CR::Why::OutputMerged,
                   "%s '%ls' type %u written to the host terminal, claim holder %u", o.clear ? "reset" : "print",
                   Printable(o.text).c_str(), static_cast<unsigned>(o.type),
                   static_cast<unsigned>(DO::HolderOf(kSatKey)));
}

// Write the owed entries in their order while the host's terminal is up; one whose call does not run stays at the
// head, tried again, and dropped (said) after kOwedAttempts runs of the call refused.
void FlushOwed() {
    if (g_owed.empty()) return;
    void* vis = SC::LocalTerminal();
    if (!vis) return;
    while (!g_owed.empty()) {
        Owed& o = g_owed.front();
        if (ApplyOwed(vis, o)) {
            FinishOwed(o);
            g_owed.pop_front();
            continue;
        }
        if (++o.attempts >= kOwedAttempts) {
            CR::Report(CR::Site::SatMerge, CR::Why::DispatchFailed,
                       "the host terminal %p refused a %s %d times -- dropped: '%ls'", vis, o.clear ? "reset" : "print",
                       o.attempts, Printable(o.text).c_str());
            g_owed.pop_front();
            continue;
        }
        return;
    }
}

// A print or reset for the shared screen: written at once while nothing is owed and the terminal is up, else kept in
// order behind what is owed and written when the terminal is. Never lost while the session lasts, within the cap.
void SubmitOwed(Owed o) {
    if (g_owed.empty()) {
        if (void* vis = SC::LocalTerminal()) {
            if (ApplyOwed(vis, o)) {
                FinishOwed(o);
                return;
            }
            ++o.attempts;
            CR::Report(CR::Site::SatMerge, CR::Why::DispatchFailed,
                       "the host terminal %p's writeToLog did not run for '%ls' -- kept, tried again", vis,
                       Printable(o.text).c_str());
        } else {
            Note(Reason::NoScreenTerminal);
            ReportNoScreen(CR::Site::SatMerge, "a print is kept for the terminal");
        }
    }
    if (g_owed.size() >= kOwedCap) {
        if (!g_saidOwedFull) {
            g_saidOwedFull = true;
            UE_LOGW("sat_console: %zu prints are owed to the host's terminal and it is still not up -- the oldest dropped",
                    kOwedCap);
        }
        g_owed.pop_front();
    }
    Note(Reason::PrintKept);
    g_owed.push_back(std::move(o));
    FlushOwed();
}

// A line printed on a terminal of the host's own is on the shared screen: written to the host's terminal, whose
// own append and trim make it the canonical text. While that terminal is not up the line is kept and written in
// its order when it is (SubmitOwed). Always accepted.
bool MergeToScreen(const std::wstring& text, uint8_t type) {
    Owed o;
    o.text = text.size() > V::kMaxPrintChars ? text.substr(0, V::kMaxPrintChars) : text;
    o.type = type;
    SubmitOwed(std::move(o));
    return true;
}

// A line the host will not run is answered as the game answers one it cannot take or run: its echo,
// then the reason's line, on the shared screen. The typist already consumed it. With the host's terminal not up the
// answer is also sent to the typist as a screen of its own, so it is not left waiting on one.
void AnswerRefused(uint8_t slot, const V::Line& in, const wchar_t* why) {
    MergeToScreen(in.line, 1);
    MergeToScreen(why, 0);
    if (!SC::LocalTerminal()) SendNoticeScreen(slot, in, why);
}

// The prompt the console's holder stands at, put on the host's terminal. A dish that does not resolve yet is
// tried again with the next packet.
bool ApplyVisibleContext(void* vis, int32_t dish, const std::wstring& name) {
    std::wstring curName;
    int32_t curDish = V::kNoDish;
    if (!ReadContext(vis, curName, curDish)) return false;
    if (curDish == dish && _wcsicmp(curName.c_str(), name.c_str()) == 0) return true;
    void* d = dish >= 0 ? ue_wrap::dish::DishByIndex(dish) : nullptr;
    if (dish >= 0 && !d) {
        Note(Reason::NoContext);
        return false;
    }
    Applying guard;
    if (!SC::CallInit(vis, name, false, d)) {
        Note(Reason::NoContext);
        return false;
    }
    g_screenDirty = true;
    return true;
}

// Unpin a terminal and stop what it runs; its typist, if still here, is told it is no longer busy.
void Discard(Terminal& t) {
    void* term = t.pin.Raw();
    if (t.slot >= 0 && t.busySent) SendBusy(static_cast<uint8_t>(t.slot), false);
    t.pin.Release();
    SC::DiscardTerminal(term);
}

void DiscardAt(size_t i) {
    Discard(g_terms[i]);
    g_terms.erase(g_terms.begin() + static_cast<std::ptrdiff_t>(i));
}

// A typist back at the terminal it left: bound to it again and told its busy level, so its own terminal
// is busy while the command it left runs on, and gets that command's lines from here on.
void Rebind(Terminal& t, uint8_t slot) {
    t.slot = slot;
    bool busy = false;
    SC::ReadProcessing(t.pin.Raw(), busy);
    t.busySent = busy;
    SendBusy(slot, busy);
    ++g_counts.rebinds;
    if (busy) ++g_counts.busyRebinds;
    UE_LOGI("sat_console: HOST slot %u is back at its terminal (busy=%d)", static_cast<unsigned>(slot), busy ? 1 : 0);
}

// The typist's terminal, made on its first line. At the cap, a departed typist's idle terminal gives way;
// with none, the line is refused.
Terminal* TerminalFor(uint8_t slot, const std::string& guid) {
    if (Terminal* t = ByGuid(guid)) {
        if (t->slot != slot) Rebind(*t, slot);
        return t;
    }
    if (g_terms.size() >= kMaxTerminals) {
        for (size_t i = 0; i < g_terms.size(); ++i) {
            bool busy = true;
            if (g_terms[i].slot < 0 && SC::ReadProcessing(g_terms[i].pin.Raw(), busy) && !busy) {
                DiscardAt(i);
                break;
            }
        }
        if (g_terms.size() >= kMaxTerminals) return nullptr;
    }
    void* desk = ue_wrap::console_desk::Instance();
    void* term = desk ? SC::CreateTerminal(desk) : nullptr;
    Terminal made;
    if (!term || !made.pin.Pin(term) || !SC::WritePanel(term, desk)) {
        made.pin.Release();
        SC::DiscardTerminal(term);
        return nullptr;
    }
    made.guid = guid;
    made.world = WI::CurrentWorld();  // null while unknown: the world term then fails open
    made.slot = slot;
    g_terms.push_back(std::move(made));
    UE_LOGI("sat_console: HOST made slot %u's terminal (%zu kept)", static_cast<unsigned>(slot), g_terms.size());
    return &g_terms.back();
}

// Whether a terminal stands in the context a line names. A dish index that does not resolve yet, or a
// context whose init fails, answers the line err rather than running it against none; nothing is kept,
// so the next line with the same index tries again. An index past the host's dishes is no dish at all.
enum class Context { Ready, NotYet, Bad };

Context ApplyContext(Terminal& t, int32_t dish, const std::wstring& name) {
    if (t.haveContext && dish == t.dish && name == t.name) return Context::Ready;
    void* d = nullptr;
    if (dish >= 0) {
        d = ue_wrap::dish::DishByIndex(dish);
        if (!d) {
            const int32_t count = ue_wrap::dish::Count();
            return count > 0 && dish >= count ? Context::Bad : Context::NotYet;
        }
    }
    // An init that fails may have set part of the context: nothing is kept, so the next line inits again.
    t.haveContext = false;
    if (!SC::CallInit(t.pin.Raw(), name, false, d)) return Context::NotYet;
    t.haveContext = true;
    t.dish = dish;
    t.name = name;
    return Context::Ready;
}

void HostRunLine(uint8_t slot, const V::Line& in);

// The kept lines, run in the order they came, once the host's terminal is up. Each is taken off the queue before it
// runs, so none can run twice.
void DrainWaiting() {
    while (!g_waiting.empty()) {
        WaitingLine w = std::move(g_waiting.front());
        g_waiting.pop_front();
        HostRunLine(w.slot, w.line);
    }
}

// Each tick: run the kept lines if the terminal is up; else answer those that have waited kLineWaitMax not ready,
// on the shared screen (kept for the terminal) and to the typist directly.
void TickWaiting(Clock::time_point now) {
    if (g_waiting.empty()) return;
    if (SC::LocalTerminal()) {
        DrainWaiting();
        return;
    }
    while (!g_waiting.empty() && now - g_waiting.front().since >= kLineWaitMax) {
        WaitingLine w = std::move(g_waiting.front());
        g_waiting.pop_front();
        Note(Reason::LineNotReady, w.line.line);
        ReportNoScreen(CR::Site::SatLine, "the line waited for the terminal and is answered not ready");
        AnswerRefused(w.slot, w.line, kNotReadyLine);
    }
}

// A client's line, run on the typist's terminal, answered, or refused: the host's own terminal is up. Whatever this
// does with the line is final -- it is run once or answered, never kept to run later.
void HostRunLine(uint8_t slot, const V::Line& in) {
    const int32_t dish = in.dish;
    const std::wstring& name = in.name;
    const std::wstring& used = in.used;
    const std::wstring& line = in.line;
    const std::string& guid = coop::player_handshake::GuidForSlot(slot);
    Terminal* t = guid.empty() ? nullptr : TerminalFor(slot, guid);
    if (!t) {
        if (!g_saidNoTerminal[slot]) {
            g_saidNoTerminal[slot] = true;
            UE_LOGW("sat_console: HOST no terminal for slot %u (%s) -- its lines are answered err",
                    static_cast<unsigned>(slot), guid.empty() ? "its identity not in yet" : "none could be made");
        }
        AnswerRefused(slot, in, kErrLine);
        return;
    }
    const auto now = Clock::now();
    if (now - t->windowStart >= kWindow) {
        t->windowStart = now;
        t->lines = 0;
    }
    const bool spawns = T::SpawnsEveryCall(line);
    if (++t->lines > kLinesPerWindow || (spawns && now < t->nextSpawnOk)) {
        AnswerRefused(slot, in, kBusyLine);  // the terminal cannot take the line, as the game says it
        return;
    }
    // A terminal still running a command keeps its context: its latent continuation reads the dish,
    // name and panel it started with. The game's own terminal refuses a line while busy, so does this.
    bool busy = false;
    const bool readOk = SC::ReadProcessing(t->pin.Raw(), busy);
    if (!readOk || busy) {
        AnswerRefused(slot, in, readOk ? kBusyLine : kErrLine);   // an unreadable flag is not "busy"
        return;
    }
    // The line runs only in the context it names. One that cannot be set up is answered err and not
    // retried: a command with an effect in the world must not run twice.
    const Context ctx = ApplyContext(*t, dish < V::kNoDish ? V::kNoDish : dish, name);
    if (ctx != Context::Ready) {
        if (!g_saidNoContext[slot]) {
            g_saidNoContext[slot] = true;
            UE_LOGW("sat_console: HOST no context for slot %u's line (dish %d: %s) -- answered err",
                    static_cast<unsigned>(slot), dish,
                    ctx == Context::Bad ? "past the host's dishes" : "not resolved, or the init failed");
        }
        AnswerRefused(slot, in, kErrLine);
        return;
    }
    // The panel the typist last used: none is a real answer (ROOT), a name that does not resolve here
    // is not, and the line does not run against no panel in its place.
    void* panel = used.empty() ? nullptr : SC::FindPanelByName(used);
    if ((!used.empty() && !panel) || !SC::WriteUsed(t->pin.Raw(), panel)) {
        if (!g_saidNoContext[slot]) {
            g_saidNoContext[slot] = true;
            UE_LOGW("sat_console: HOST no panel '%ls' for slot %u's line -- answered err",
                    Printable(used).c_str(), static_cast<unsigned>(slot));
        }
        AnswerRefused(slot, in, kErrLine);
        return;
    }
    if (spawns) t->nextSpawnOk = now + kSpawnEvery;
    UE_LOGI("sat_console: HOST runs '%ls' for slot %u (dish %d)", Printable(line).c_str(),
            static_cast<unsigned>(slot), dish);
    // What the run did to the host's screen, read around it: the typist's terminal's busy level, the canonical log,
    // and what was owed to it.
    void* vis = SC::LocalTerminal();
    std::wstring logBefore, logAfter;
    const bool haveBefore = vis && SC::ReadLog(vis, logBefore);
    const size_t owedBefore = g_owed.size();
    bool busyBefore = false, busyAfter = false;
    SC::ReadProcessing(t->pin.Raw(), busyBefore);
    if (!SC::RunCommand(t->pin.Raw(), line)) {
        CR::Report(CR::Site::SatLine, CR::Why::DispatchFailed,
                   "slot %u '%ls': enterCommand did not run on terminal %p (host terminal %p, claim holder %u)",
                   static_cast<unsigned>(slot), Printable(line).c_str(), t->pin.Raw(), vis,
                   static_cast<unsigned>(DO::HolderOf(kSatKey)));
        AnswerRefused(slot, in, kErrLine);
        return;
    }
    ++g_counts.linesRun;
    SC::ReadProcessing(t->pin.Raw(), busyAfter);
    const bool haveAfter = vis && SC::ReadLog(vis, logAfter);
    const bool printed = (haveBefore && haveAfter && logAfter != logBefore) || g_owed.size() > owedBefore;
    const CR::Why why = printed ? CR::Why::OutputMerged
                                : (busyAfter ? CR::Why::WaitingContinuation : CR::Why::NativeNoOp);
    CR::Report(CR::Site::SatLine, why,
               "slot %u '%ls': typist terminal %p busy %d->%d, host terminal %p log %s%s, claim holder %u, %zu print(s) "
               "owed%s", static_cast<unsigned>(slot), Printable(line).c_str(), t->pin.Raw(), busyBefore ? 1 : 0,
               busyAfter ? 1 : 0, vis, printed ? "moved" : "unchanged", busyAfter ? ", a continuation runs" : "",
               static_cast<unsigned>(DO::HolderOf(kSatKey)), g_owed.size(),
               why == CR::Why::NativeNoOp ? " (it printed nothing and left nothing running: a world effect only, or a no-op)" : "");
}

// A client's line. Refused at once when it is not one the host runs; run when the host's own terminal -- the screen
// its output goes to -- is up; else kept, unrun and in order, and run when the terminal is, or answered not ready.
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
    if (SC::LocalTerminal()) {
        DrainWaiting();  // lines kept before this one run first: a typist's lines keep their order
        HostRunLine(slot, in);
        return;
    }
    size_t mine = 0;
    for (const WaitingLine& w : g_waiting)
        if (w.slot == slot) ++mine;
    if (mine >= kMaxWaitingPerSlot) {
        AnswerRefused(slot, in, kBusyLine);
        return;
    }
    Note(Reason::LineKept, in.line);
    ReportNoScreen(CR::Site::SatLine, "the line is kept, unrun, until the terminal is up");
    WaitingLine w;
    w.slot = slot;
    w.line = in;
    w.since = Clock::now();
    g_waiting.push_back(std::move(w));
}

bool InputNewer(const V::Input& a, const V::Input& b) {
    return a.stamp > b.stamp || (a.stamp == b.stamp && a.seq > b.seq);
}

// The holder's input, put on the host's terminal and relayed to the others; taken (acked) here and no sooner.
void HostApplyInput(uint8_t slot, const V::Input& in, void* vis) {
    g_slotStamp[slot] = in.stamp;
    g_slotInSeq[slot] = in.seq;
    const std::wstring text = V::SanitizeInput(in.text);
    ApplyVisibleContext(vis, in.dish, in.name);
    std::wstring cur;
    if (!ReadHolderInput(vis, cur) || cur != text) {
        Applying guard;
        if (!SC::WriteInput(vis, text))
            CR::Report(CR::Site::SatInput, CR::Why::DispatchFailed,
                       "slot %u's %zu-char input could not be written to the host terminal %p",
                       static_cast<unsigned>(slot), text.size(), vis);
        else if (CR::TraceOn())
            CR::Report(CR::Site::SatInput, CR::Why::StateChanged,
                       "slot %u's %zu-char input written to the host terminal %p, claim holder %u",
                       static_cast<unsigned>(slot), text.size(), vis, static_cast<unsigned>(DO::HolderOf(kSatKey)));
    }
    ++g_counts.inputsTaken;
    V::InputRelay r;
    r.holder = slot;
    r.gen = g_gen;
    r.seq = in.seq;
    r.text = text;
    ForEachClient([&](uint8_t to) {
        if (to == slot) return;
        std::vector<uint8_t> b;
        V::EncodeInputRelay(b, r);
        EnqueueLatest(to, std::move(b));
    });
    if (Trace())
        UE_LOGI("sat_console: HOST input from slot %u: stamp %u seq %u, %zu chars", static_cast<unsigned>(slot),
                in.stamp, in.seq, text.size());
}

// The holder's input line. Refused unless the sender is the claim table's holder of the console, and unless it is
// newer than the last taken from that slot. While the host's terminal is not up the newest is kept and put on it
// when it is, so the sequence is acked only after it is applied.
void HostOnInput(uint8_t slot, const V::Input& in) {
    if (DO::HolderOf(kSatKey) != slot) {
        ++g_counts.inputsStale;
        Note(Reason::NotHolder);
        return;
    }
    if (in.stamp < g_slotStamp[slot] || (in.stamp == g_slotStamp[slot] && in.seq <= g_slotInSeq[slot])) {
        ++g_counts.inputsStale;
        Note(Reason::StaleInput);
        return;
    }
    void* vis = SC::LocalTerminal();
    if (!vis) {
        Note(Reason::NoScreenTerminal);
        if (!g_heldInput.valid || g_heldInput.slot != slot || InputNewer(in, g_heldInput.in)) {
            g_heldInput.valid = true;
            g_heldInput.slot = slot;
            g_heldInput.in = in;
        }
        Note(Reason::InputKept);
        ReportNoScreen(CR::Site::SatInput, "the holder's input is kept for the terminal");
        return;
    }
    g_heldInput.valid = false;  // this one is newer than anything kept
    HostApplyInput(slot, in, vis);
}

// The kept input, applied once the host's terminal is up if its sender still holds the console and nothing newer
// from it was taken meanwhile.
void TickHeldInput() {
    if (!g_heldInput.valid) return;
    if (DO::HolderOf(kSatKey) != g_heldInput.slot) {
        g_heldInput.valid = false;
        return;
    }
    void* vis = SC::LocalTerminal();
    if (!vis) return;
    const HeldInput h = g_heldInput;
    g_heldInput.valid = false;
    if (h.in.stamp < g_slotStamp[h.slot] || (h.in.stamp == g_slotStamp[h.slot] && h.in.seq <= g_slotInSeq[h.slot])) return;
    HostApplyInput(h.slot, h.in, vis);
}

// What a command the holder ran at its own terminal printed, written to the shared screen in the order the
// typist saw it. `clr` is the shared reset. Kept, in order, while the host's terminal is not up; its sequence is
// acked when it is written.
void HostOnPresent(uint8_t slot, const V::Present& p) {
    if (DO::HolderOf(kSatKey) != slot) {
        Note(Reason::NotHolder);
        return;
    }
    if (p.seq <= std::max(g_slotPresSeq[slot], g_queuedPresSeq[slot])) {
        Note(Reason::StaleInput);
        return;
    }
    g_queuedPresSeq[slot] = p.seq;
    Owed o;
    o.clear = p.kind != V::PresentKind::Write;
    if (!o.clear) {
        o.text = p.text.size() > V::kMaxPrintChars ? p.text.substr(0, V::kMaxPrintChars) : p.text;
        o.type = p.type;
    }
    o.ackSlot = slot;
    o.ackSeq = p.seq;
    SubmitOwed(std::move(o));
}

void HostOnBlob(uint8_t slot, const std::vector<uint8_t>& blob) {
    uint8_t op = 0;
    V::Line line;
    V::Input input;
    V::Present present;
    bool ok = V::PeekOp(blob, op);
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
    } else {
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
void HostAfterBody(Terminal& t) {
    g_screenDirty = true;  // its busy level is part of the screen
    bool busy = false;
    if (t.slot < 0 || !SC::ReadProcessing(t.pin.Raw(), busy) || busy == t.busySent) return;
    t.busySent = busy;
    SendBusy(static_cast<uint8_t>(t.slot), busy);
}

// ---- the holder: typing ----

// The prompt and the text a holder shows, sent to the host (a client) or relayed to the rest (the host) when
// either moved. At the claim's rising edge the terminal already shows what the screen showed, so that is
// the baseline and nothing is sent until something moves.
void HolderTick(Clock::time_point now, bool isHost) {
    const bool holds = DO::LocalHolds(kSatKey);
    void* term = SC::LocalTerminal();
    if (!holds) {
        g_wasHolding = false;
        return;
    }
    if (!term) return;
    if (!g_wasHolding) {
        g_wasHolding = true;
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
                    "while this machine holds the console");
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
        V::InputRelay r;
        r.holder = 0;
        r.gen = g_gen;
        r.seq = seq;
        r.text = text;
        ForEachClient([&](uint8_t to) {
            std::vector<uint8_t> b;
            V::EncodeInputRelay(b, r);
            EnqueueLatest(to, std::move(b));
        });
        g_screenDirty = true;  // the prompt may have moved with it
    } else {
        V::Input in;
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
        UE_LOGI("sat_console: %s input out: stamp %u seq %u, %zu chars, prompt '%ls' dish %d",
                isHost ? "HOST" : "CLIENT", g_myStamp, seq, text.size(), Printable(name).c_str(), dish);
}

// ---- the client ----

// A line whose command rests on the shared world goes to the host, with the terminal's context as it
// stands: the dish its panel pointed it at (or ROOT), its name, and the panel last used. A busy
// terminal answers the line itself, as it would alone; so does a line the terminal owns. The body is
// refused, which leaves its own tail unrun, so the input is consumed here as that tail does
// (ui_console.enterCommand: `command = ""`, the text box emptied); the echo and every answer come back
// from the host, on the shared screen.
sg::Verdict OnEnterPre(const sg::Call& c) {
    g_pendingClear = false;
    if (!SessionIf(coop::net::Role::Client)) return sg::Verdict::Run;
    void* term = c.object;
    if (term != SC::LocalTerminal()) {
        Note(Reason::LocalNotTerminal);
        return sg::Verdict::Run;
    }
    bool busy = false;
    std::wstring line;
    if (!SC::ReadProcessing(term, busy) || !SC::ReadEnterCommandLine(c.locals, line)) {
        Note(Reason::LocalUnreadable);
        return sg::Verdict::Run;
    }
    if (busy) {
        Note(Reason::LocalBusy, line);
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
    void* dish = SC::ReadActiveDish(term);
    V::Line out;
    out.dish = dish ? ue_wrap::dish::IndexOf(dish) : V::kNoDish;
    if (out.dish < V::kNoDish) out.dish = V::kNoDish;
    out.name = name;
    out.used = SC::PanelName(SC::ReadUsed(term));
    if (out.used.size() > V::kMaxNameChars) out.used.clear();
    out.line = line;
    std::vector<uint8_t> b;
    V::EncodeLine(b, out);
    Enqueue(0, std::move(b));
    SC::ConsumeInput(term);
    ++g_counts.linesSent;
    UE_LOGI("sat_console: CLIENT '%ls' runs on the host's terminal (dish %d)", line.c_str(), out.dish);
    return sg::Verdict::Cancel;
}

// After this terminal ran a body of its own. While the host's terminal is busy the flag is the mirror's:
// a line this terminal ran itself may have cleared it, and it is set again, the line's hold over. Else
// the flag is the line's own.
void ClientAfterLocalBody(void* term) {
    bool busy = false;
    if (!SC::ReadProcessing(term, busy)) return;
    if (g_mirrorBusy) {
        if (!busy) {
            g_localHolds = false;
            SC::WriteProcessing(term, true);
        }
    } else {
        g_localHolds = busy;
    }
}

void SendPresent(V::PresentKind kind, uint8_t type, const std::wstring& text) {
    V::Present p;
    p.seq = ++g_presSeq;
    p.kind = kind;
    p.type = type > V::kMaxBrackets ? V::kMaxBrackets : type;
    p.text = text.size() > V::kMaxPrintChars ? text.substr(0, V::kMaxPrintChars) : text;
    std::vector<uint8_t> b;
    V::EncodePresent(b, p);
    Enqueue(0, std::move(b));
    ++g_counts.presentsSent;
}

bool EqualNoCase(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// Show a screen on this machine's terminal. The typist (the claim's holder) keeps its own prompt, input and
// busy level, which are the source of the others'; it shows the history. A replica takes everything. None of it
// runs a command, and none of it prints: the history is written to the slot and repainted by the terminal's
// own fixLog, and the handlers our own writes wake are held off by `g_applying`.
bool ApplyScreen(void* term, const V::Screen& s, bool holding) {
    Applying guard;
    bool ok = true;
    if (!holding) {
        std::wstring curName;
        int32_t curDish = V::kNoDish;
        if (ReadContext(term, curName, curDish) && (curDish != s.dish || !EqualNoCase(curName, s.name))) {
            void* d = s.dish >= 0 ? ue_wrap::dish::DishByIndex(s.dish) : nullptr;
            if (s.dish >= 0 && !d) Note(Reason::NoContext);
            else if (!SC::CallInit(term, s.name, false, d)) Note(Reason::NoContext);
        }
    }
    std::wstring curLog;
    if (!SC::ReadLog(term, curLog) || curLog != s.log) ok = SC::WriteLog(term, s.log) && ok;
    if (!holding) {
        std::wstring curIn;
        const std::wstring input = V::SanitizeInput(s.input);
        if (!ReadHolderInput(term, curIn) || curIn != input) ok = SC::WriteInput(term, input) && ok;
        g_mirrorBusy = s.busy != 0;
        if (!g_localHolds) SC::WriteProcessing(term, g_mirrorBusy);
    }
    return ok;
}

// A screen kept back is counted once however many ticks it waits.
void NoteHeld() {
    if (g_heldCounted) return;
    g_heldCounted = true;
    ++g_counts.screensHeld;
    Note(Reason::ScreenHeld);
}

void TryApplyPending(Clock::time_point now) {
    if (!g_havePending) return;
    void* term = SC::LocalTerminal();
    const bool holding = DO::LocalHolds(kSatKey);
    if (!term) {
        NoteHeld();
        return;
    }
    // The typist's own printed lines are on this terminal and on their way to the host: a screen the host made
    // before it merged them would drop them for a moment, so it waits for the host's word, or a grace.
    if (holding && g_pending.ackPresent < g_presSeq && now - g_pendingSince < kHeldGrace) {
        NoteHeld();
        return;
    }
    if (!ApplyScreen(term, g_pending, holding)) return;  // the slot would not take it: kept, tried again
    g_havePending = false;
    g_heldCounted = false;
    ++g_counts.screensApplied;
    if (Trace())
        UE_LOGI("sat_console: CLIENT screen rev %u applied (%zu chars, prompt '%ls' dish %d, holder %u gen %u%s)",
                g_pending.rev, g_pending.log.size(), Printable(g_pending.name).c_str(), g_pending.dish,
                static_cast<unsigned>(g_pending.holder), g_pending.gen, holding ? ", typist: history only" : "");
}

void ClientOnScreen(const V::Screen& s) {
    if (s.gen < g_lastGen) return;  // an older generation of the claim than one seen
    g_lastGen = s.gen;
    g_presAcked = std::max(g_presAcked, s.ackPresent);
    if (Trace())
        UE_LOGI("sat_console: CLIENT screen rev %u received (%zu chars, holder %u gen %u, acked present %u input %u)",
                s.rev, s.log.size(), static_cast<unsigned>(s.holder), s.gen, s.ackPresent, s.ackInput);
    if (!g_havePending) g_pendingSince = Clock::now();
    g_heldCounted = false;
    g_pending = s;
    g_havePending = true;
    TryApplyPending(Clock::now());
}

void ClientOnRelay(const V::InputRelay& r) {
    if (DO::LocalHolds(kSatKey)) return;  // this machine's own typing is the source
    if (r.gen < g_lastGen) return;
    g_lastGen = r.gen;
    void* term = SC::LocalTerminal();
    if (!term) return;  // the next screen carries the text
    const std::wstring text = V::SanitizeInput(r.text);
    std::wstring cur;
    if (ReadHolderInput(term, cur) && cur == text) return;
    Applying guard;
    SC::WriteInput(term, text);
    if (Trace())
        UE_LOGI("sat_console: CLIENT input from holder %u applied: seq %u, %zu chars", static_cast<unsigned>(r.holder),
                r.seq, text.size());
}

void ClientOnBlob(const std::vector<uint8_t>& blob) {
    uint8_t op = 0;
    if (!V::PeekOp(blob, op)) return;
    if (op == V::kOpBusy) {
        bool busy = false;
        if (!V::DecodeBusy(blob, busy)) {
            Note(Reason::BadPacket);
            return;
        }
        g_mirrorBusy = busy;
        void* term = SC::LocalTerminal();
        if (term && (g_mirrorBusy || !g_localHolds)) SC::WriteProcessing(term, g_mirrorBusy);
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

sg::Verdict OnWritePre(const sg::Call& c) {
    if (SessionIf(coop::net::Role::Host)) {
        Terminal* t = ByObject(c.object);
        if (!t) return sg::Verdict::Run;  // the host's own terminal: its post tells the screen moved
        std::wstring text;
        uint8_t type = 0;
        if (!SC::ReadWriteToLogArgs(c.locals, text, type)) return sg::Verdict::Run;
        // What the typist's terminal prints is on the shared screen, whether or not the typist is still here.
        if (MergeToScreen(text, type)) ++g_counts.linesReturned;
        return sg::Verdict::Run;
    }
    if (SessionIf(coop::net::Role::Client)) {
        if (c.object != SC::LocalTerminal() || g_applying) return sg::Verdict::Run;
        // A print inside a command this terminal runs is that command's, and goes to the host to be merged. One
        // outside any is the world's own alert, which the host's terminal gets too and shows back: a second copy
        // here would be the only line the others lack.
        if (!SC::InTerminalBody(c.object)) {
            Note(Reason::AlertSuppressed);
            return sg::Verdict::Cancel;
        }
        std::wstring text;
        uint8_t type = 0;
        if (SC::ReadWriteToLogArgs(c.locals, text, type)) SendPresent(V::PresentKind::Write, type, text);
    }
    return sg::Verdict::Run;
}

void OnWritePost(const sg::Call& c) {
    if (SessionIf(coop::net::Role::Host) && c.object == SC::LocalTerminal()) g_screenDirty = true;
}

void OnBodyPost(const sg::Call& c) {
    if (SessionIf(coop::net::Role::Host)) {
        if (Terminal* t = ByObject(c.object)) {
            HostAfterBody(*t);
        } else if (c.object == SC::LocalTerminal()) {
            g_screenDirty = true;  // the host's own terminal ran a command, or a latent line of one
            if (c.tag == kTagEnter) {
                g_inputDirty = true;  // a submit empties the input: sent at once
                g_nextInput = Clock::time_point{};
            }
        }
    } else if (SessionIf(coop::net::Role::Client)) {
        if (c.object != SC::LocalTerminal()) return;
        ClientAfterLocalBody(c.object);
        if (c.tag == kTagEnter) {
            if (g_pendingClear) {
                g_pendingClear = false;
                SendPresent(V::PresentKind::Clear, 0, std::wstring());
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

// The core three stand or fall together: a line refused on a client whose answers no watch would put on the
// screen would vanish. The input watches are extra: with the changed-text handler unwatched the holder's input
// is polled instead.
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
    const bool changed = sg::WatchName(kRowTextChanged, kTagChanged, OnTextChangedPre, OnInputPost);
    const bool type = sg::WatchName(kRowTypeSymbol, kTagType, nullptr, OnInputPost);
    const bool key = sg::WatchName(kRowKeyDown, kTagKey, nullptr, OnInputPost);
    if (!(changed && type && key))
        UE_LOGW("sat_console: an input watch did not register (changed=%d typeSymbol=%d keyDown=%d) -- the input is "
                "polled while a machine holds the console", changed ? 1 : 0, type ? 1 : 0, key ? 1 : 0);
}

// The screen state belongs to its world: with a new one, what was last published and what is held are over, and
// the host's screen goes out again from its new terminal.
void OnWorldChanged() {
    g_pubValid = false;
    g_havePending = false;
    g_screenDirty = true;
    g_wasHolding = false;
    g_ctxSentValid = false;
    g_pendingClear = false;
    if (g_holding) SC::HoldOffscreenTick(false);  // the old world's component is no more: nothing of its to put back
    g_holding = false;
    g_holdUntil = Clock::time_point{};
    UE_LOGI("sat_console: the world changed -- the screen is read again from this world's terminal");
}

// The desk's widget component ticks offscreen while a remote typist holds the console, a line waits or a typist's
// terminal runs a command, and for kTickLinger after: nobody on the host looks at the screen the work is done on.
void TickOffscreen(Clock::time_point now) {
    bool want = (g_holder != V::kNoHolder && g_holder != 0) || !g_waiting.empty() || !g_owed.empty() ||
                g_heldInput.valid;
    if (!want)
        for (const Terminal& t : g_terms) {
            bool busy = false;
            if (SC::ReadProcessing(t.pin.Raw(), busy) && busy) {
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

void HostTick(Clock::time_point now) {
    const uint8_t holder = DO::HolderOf(kSatKey);
    if (holder != g_holder) {
        g_holder = holder;
        ++g_gen;
        g_screenDirty = true;
        UE_LOGI("sat_console: HOST the SAT console is %s (generation %u)",
                holder == V::kNoHolder ? "free" : ("held by slot " + std::to_string(static_cast<unsigned>(holder))).c_str(),
                g_gen);
    }
    for (int i = 1; i < kMaxPeers; ++i)
        if (g_owedScreen[i]) SendWholeScreen(static_cast<uint8_t>(i));
    FlushOwed();
    TickWaiting(now);
    TickHeldInput();
    HolderTick(now, true);
    TickOffscreen(now);
    if (g_screenDirty && now >= g_nextScreen) {
        g_screenDirty = false;  // cleared before the publish, so a body that runs during it keeps it dirty
        g_nextScreen = now + kScreenEvery;
        if (!HostPublish(false)) {
            g_screenDirty = true;  // not published: still owed, tried again
            g_nextScreen = now + kPublishRetry;
        }
    }
}

void ClientTick(Clock::time_point now) {
    HolderTick(now, false);
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
    // A terminal holds its world's desk, dish and panel: one made in a world that is known and no longer
    // the current one goes. An unknown world keeps it.
    if (void* world = WI::CurrentWorld()) {
        if (world != g_world) {
            if (g_world) OnWorldChanged();
            g_world = world;
        }
        for (size_t i = 0; i < g_terms.size();) {
            if (!g_terms[i].world || g_terms[i].world == world) {
                ++i;
                continue;
            }
            UE_LOGI("sat_console: HOST a terminal discarded with its world");
            DiscardAt(i);
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
        if (Terminal* t = ByGuid(guid))
            if (t->slot != slot) Rebind(*t, static_cast<uint8_t>(slot));
    // A peer new to the session, or back, is shown the whole screen, even when nothing has moved since.
    SendWholeScreen(static_cast<uint8_t>(slot));
}

void OnPeerGone(uint8_t slot) {
    if (slot >= kMaxPeers) return;
    g_asm.ClearSlot(slot);
    g_out[slot] = Outbox{};
    g_saidRefused[slot] = g_saidMalformed[slot] = g_saidNoTerminal[slot] = g_saidNoContext[slot] = false;
    g_slotStamp[slot] = g_slotInSeq[slot] = g_slotPresSeq[slot] = 0;
    g_queuedPresSeq[slot] = 0;
    g_owedScreen[slot] = false;
    // Its lines that never ran are not run for a typist that is gone (a line is run for its sender, once); what it
    // printed is still owed to the screen, but no longer acknowledged to a slot that may have another occupant.
    for (auto it = g_waiting.begin(); it != g_waiting.end();) it = (it->slot == slot) ? g_waiting.erase(it) : it + 1;
    for (Owed& o : g_owed)
        if (o.ackSlot == slot) o.ackSlot = -1;
    if (g_heldInput.valid && g_heldInput.slot == slot) g_heldInput.valid = false;
    for (Terminal& t : g_terms) {
        if (t.slot != slot) continue;
        t.slot = -1;  // its command, if any, runs to its end; the terminal waits for its typist
        UE_LOGI("sat_console: HOST slot %u left -- its terminal kept", static_cast<unsigned>(slot));
    }
}

void OnDisconnect() {
    // A busy flag this terminal holds only as the host's mirror goes with the session.
    if (g_mirrorBusy && !g_localHolds)
        if (void* term = SC::LocalTerminal()) SC::WriteProcessing(term, false);
    g_mirrorBusy = g_localHolds = false;
    while (!g_terms.empty()) DiscardAt(g_terms.size() - 1);
    for (auto& o : g_out) o = Outbox{};
    for (bool& b : g_saidRefused) b = false;
    for (bool& b : g_saidMalformed) b = false;
    for (bool& b : g_saidNoTerminal) b = false;
    for (bool& b : g_saidNoContext) b = false;
    for (bool& b : g_owedScreen) b = false;
    for (uint32_t& v : g_slotStamp) v = 0;
    for (uint32_t& v : g_slotInSeq) v = 0;
    for (uint32_t& v : g_slotPresSeq) v = 0;
    for (uint32_t& v : g_queuedPresSeq) v = 0;
    g_waiting.clear();
    g_owed.clear();
    g_saidOwedFull = false;
    g_heldInput = HeldInput{};
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
    g_pubValid = false;
    g_pub = V::Screen{};
    g_rev = 0;
    g_gen = 0;
    g_holder = V::kNoHolder;
    g_screenDirty = false;
    g_wasHolding = false;
    g_inputDirty = false;
    g_myStamp = 0;
    g_inSeq = 0;
    g_ctxSentValid = false;
    g_lastInputSent.clear();
    g_presSeq = g_presAcked = 0;
    g_pendingClear = false;
    g_havePending = false;
    g_lastGen = 0;
    g_world = nullptr;
    g_saidPoll = false;
}

Counts LaneCounts() {
    return g_counts;
}

size_t KeptTerminals(void** out, size_t cap) {
    size_t n = 0;
    for (const Terminal& t : g_terms)
        if (n < cap) out[n++] = t.pin.Raw();
    return n;
}

}  // namespace coop::sat_console_sync

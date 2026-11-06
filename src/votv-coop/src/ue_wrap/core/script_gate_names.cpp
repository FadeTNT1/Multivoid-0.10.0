// ue_wrap/core/script_gate_names.cpp -- the script gate's name watches: registration by a row's names,
// the resolve that turns those names into keys on the game thread, and the judging of a class-scoped
// watch's class. The tables are script_gate.cpp's, shared through script_gate_detail.h.

#include "ue_wrap/core/script_gate.h"

#include "script_gate_detail.h"

#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cwchar>

namespace ue_wrap::script_gate {
using namespace detail;

namespace GT = ue_wrap::game_thread;
namespace P  = ue_wrap::profile;

namespace {

inline bool SameLiteral(const wchar_t* a, const wchar_t* b) {
    return a == b || (a && b && std::wcscmp(a, b) == 0);
}

// A name watch waiting for its FName. It lives here, not in the table: the call path never reads this
// list, so the resolve frees a record the moment its watch reaches the table, and a watch takes one keyed
// slot for the life of the process. A record also stays, marked dead, for a watch that never reached the
// table (its class declares no such body, or the table was full), so the settled question has its answer
// and watching it again finds it rather than taking another record. Under the registration mutex.
struct PendingWatch {
    bool used = false;
    bool enabled = false;
    bool dead = false;
    int tag = 0;
    PreFn pre = nullptr;
    PostFn post = nullptr;
    const wchar_t* name = nullptr;
    const wchar_t* className = nullptr;
};
PendingWatch g_pending[kMaxPending];

inline bool SameWatch(const wchar_t* eName, const wchar_t* eClass, int eTag, PreFn ePre, PostFn ePost,
                      const wchar_t* className, const wchar_t* name, int tag, PreFn pre, PostFn post) {
    return eName && eTag == tag && ePre == pre && ePost == post && std::wcscmp(eName, name) == 0 &&
           SameLiteral(eClass, className);
}

int PendingUsed() {
    int n = 0;
    for (const PendingWatch& r : g_pending) n += r.used ? 1 : 0;
    return n;
}

// A refusal for want of room is a gate that silently never fires, so every one is said, not only the first.
// Bounded so a consumer that retries from its tick cannot flood the log.
void LogNoRoom(const char* where, const wchar_t* className, const wchar_t* name) {
    static std::atomic<int> s_said{0};
    const int n = s_said.fetch_add(1, std::memory_order_relaxed);
    if (n < 64)
        UE_LOGE("script_gate: REQUIRED WATCH LOST -- %s is full (%d/%d keyed name slots, %d waiting, %d/%d pending "
                "records) -- cannot watch '%ls'%ls%ls; this gate will never fire. Raise kSlotBits in "
                "script_gate_detail.h", where, g_nameKeyed, kMaxKeyed, g_namesPending.load(std::memory_order_relaxed),
                PendingUsed(), kMaxPending, name, className ? L" of class " : L"", className ? className : L"");
    else if (n == 64)
        UE_LOGE("script_gate: more than 64 name watches lost for want of room; later ones are not logged");
}

// The settle summary: every name watch the lanes asked for, live or dead, with the table's load. Said each
// time the pending list drains, so the boot's whole watch set is reported in one line, and again for a later
// batch. A dead watch is a gate that never fires: the line is an ERROR then, with the count, so a regression
// that kills watches (the b288 full table took 64 of them, every door, toggle and keypad lane) cannot hide
// among the INFO lines. Takes the registration mutex.
void LogSettleSummary() {
    std::lock_guard<std::mutex> lk(g_regMutex);
    int live = 0, retired = 0, dead = 0;
    for (const Entry& e : g_nameTable) {
        if (e.key.load(std::memory_order_relaxed) == 0 || !e.name) continue;
        if (e.dead.load(std::memory_order_relaxed)) ++dead;
        else if (e.enabled.load(std::memory_order_relaxed)) ++live;
        else ++retired;
    }
    int waiting = 0;
    for (const PendingWatch& r : g_pending) {
        if (!r.used) continue;
        if (r.dead) ++dead;
        else ++waiting;
    }
    const int asked = live + dead + waiting;
    const int fnLive = g_fnWatches.load(std::memory_order_relaxed);
    if (dead == 0)
        UE_LOGI("script_gate: %d/%d watches live, 0 dead (%d retired, %d waiting; name table %d/%d keyed; %d exact "
                "watch(es), table %d/%d keyed)", live, asked, retired, waiting, g_nameKeyed, kMaxKeyed, fnLive,
                g_fnKeyed, kMaxKeyed);
    else
        UE_LOGE("script_gate: %d/%d watches live, %d DEAD -- each dead watch is a gate that never fires (the lines "
                "above name them) (%d retired, %d waiting; name table %d/%d keyed; %d exact watch(es), table %d/%d "
                "keyed)", live, asked, dead, retired, waiting, g_nameKeyed, kMaxKeyed, fnLive, g_fnKeyed, kMaxKeyed);
    if (g_nameKeyed >= kKeyedWarn || g_fnKeyed >= kKeyedWarn)
        UE_LOGE("script_gate: a watch table is past three quarters full (name %d, exact %d, of %d) -- raise kSlotBits "
                "in script_gate_detail.h before watches start dying", g_nameKeyed, g_fnKeyed, kMaxKeyed);
}

// A name watch, class-scoped when `className` is set. A name's key is its FName, unknown until the
// game thread converts it; until then the watch waits in the pending list, and the resolve moves it
// into the table.
bool WatchNameScoped(const wchar_t* className, const wchar_t* name, int tag, PreFn pre, PostFn post) {
    if (!name || !*name || (className && !*className) || (!pre && !post)) return false;
    if (RefusedUninstalled("WatchName")) return false;
    std::lock_guard<std::mutex> lk(g_regMutex);
    // In the table already: re-enable it. A dead one stays dead (its class declares no such body) and
    // answers true, as a fresh registration would before its resolve killed it the same way, without
    // taking a record or a slot each time a consumer retries it.
    for (int i = 0; i < kSlots; ++i) {
        Entry& e = g_nameTable[i];
        if (e.key.load(std::memory_order_relaxed) == 0 ||
            !SameWatch(e.name, e.className, e.tag, e.pre, e.post, className, name, tag, pre, post))
            continue;
        if (e.dead.load(std::memory_order_relaxed)) return true;
        if (!e.enabled.exchange(true, std::memory_order_release)) g_nameWatches.fetch_add(1, std::memory_order_release);
        return true;
    }
    // Waiting for its name, or dead before it reached the table: the same record.
    PendingWatch* free = nullptr;
    for (PendingWatch& r : g_pending) {
        if (!r.used) {
            if (!free) free = &r;
            continue;
        }
        if (!SameWatch(r.name, r.className, r.tag, r.pre, r.post, className, name, tag, pre, post)) continue;
        if (!r.dead) r.enabled = true;
        return true;
    }
    // A new watch. Refused now when the table could not take it at its resolve (every waiting watch holds
    // a reservation), so the consumer hears it at once rather than through a watch that settles dead.
    if (!free) {
        LogNoRoom("the pending list", className, name);
        return false;
    }
    if (g_nameKeyed + g_namesPending.load(std::memory_order_relaxed) >= kMaxKeyed) {
        LogNoRoom("the name table", className, name);
        return false;
    }
    *free = PendingWatch{true, true, false, tag, pre, post, name, className};
    g_namesPending.fetch_add(1, std::memory_order_release);
    if (className)
        UE_LOGI("script_gate: watching '%ls' of class '%ls' tag=%d -- pending the game-thread name resolve",
                name, className, tag);
    else
        UE_LOGI("script_gate: watching '%ls' tag=%d -- pending the game-thread name resolve", name, tag);
    GT::Post([] { ResolvePendingNames(); });
    return true;
}

// The table only: a watch still pending is not live, nor is a dead one.
bool ScopedWatchLive(const wchar_t* className, const wchar_t* name, int tag) {
    if (!name) return false;
    for (int i = 0; i < kSlots; ++i) {
        const Entry& e = g_nameTable[i];
        if (e.key.load(std::memory_order_acquire) == 0) continue;
        if (e.name != name || e.className != className || e.tag != tag) continue;
        if (e.enabled.load(std::memory_order_acquire) && e.resolved.load(std::memory_order_acquire) &&
            !e.dead.load(std::memory_order_acquire))
            return true;
    }
    return false;
}

// Live, or dead for good: in the table with the dead mark, or a pending record that never got there.
bool ScopedWatchSettled(const wchar_t* className, const wchar_t* name, int tag) {
    if (!name) return false;
    for (int i = 0; i < kSlots; ++i) {
        const Entry& e = g_nameTable[i];
        if (e.key.load(std::memory_order_acquire) == 0) continue;
        if (e.name != name || e.className != className || e.tag != tag) continue;
        if (e.dead.load(std::memory_order_acquire)) return true;
        if (e.enabled.load(std::memory_order_acquire) && e.resolved.load(std::memory_order_acquire))
            return true;
    }
    std::lock_guard<std::mutex> lk(g_regMutex);
    for (const PendingWatch& r : g_pending)
        if (r.used && r.dead && r.name == name && r.className == className && r.tag == tag) return true;
    return false;
}

}  // namespace

namespace {
// Disable that name watch: its table entry, or its pending record, which the resolve then moves into the
// table disabled, its literals kept for a re-watch.
bool UnwatchNameScoped(const wchar_t* className, const wchar_t* name, int tag, PreFn pre, PostFn post) {
    if (!name) return false;
    std::lock_guard<std::mutex> lk(g_regMutex);
    bool found = false;
    for (int i = 0; i < kSlots; ++i) {
        Entry& e = g_nameTable[i];
        if (e.key.load(std::memory_order_relaxed) == 0 || !e.name || e.tag != tag || e.pre != pre ||
            e.post != post || std::wcscmp(e.name, name) != 0 || !SameLiteral(e.className, className))
            continue;
        if (e.enabled.exchange(false, std::memory_order_release)) g_nameWatches.fetch_sub(1, std::memory_order_release);
        found = true;
    }
    for (PendingWatch& r : g_pending) {
        if (!r.used || !SameWatch(r.name, r.className, r.tag, r.pre, r.post, className, name, tag, pre, post)) continue;
        r.enabled = false;
        found = true;
    }
    return found;
}
}  // namespace

namespace {
// The class a name-form row scopes its watch to: none for an AnyClass row, its class for a Declared one.
bool ScopeOf(const facts::Function& row, const wchar_t*& cls) {
    if (row.GetMode() == facts::Mode::AnyClass && !row.Class()) {
        cls = nullptr;
        return true;
    }
    if (row.GetMode() == facts::Mode::Declared && row.Class()) {
        cls = row.Class();
        return true;
    }
    UE_LOGE("script-gate: row %s cannot be a name watch (an AnyClass row with no class, or a Declared one with "
            "its class)", row.Ident());
    return false;
}
}  // namespace

bool WatchName(const facts::Function& row, int tag, PreFn pre, PostFn post) {
    const wchar_t* cls = nullptr;
    const bool ok = ScopeOf(row, cls) && WatchNameScoped(cls, row.Name(), tag, pre, post);
    row.NoteUse(facts::kUseScriptGate, ok);
    return ok;
}

bool UnwatchName(const facts::Function& row, int tag, PreFn pre, PostFn post) {
    const wchar_t* cls = nullptr;
    return ScopeOf(row, cls) && UnwatchNameScoped(cls, row.Name(), tag, pre, post);
}

bool NameWatchLive(const facts::Function& row, int tag) {
    const wchar_t* cls = nullptr;
    return ScopeOf(row, cls) && ScopedWatchLive(cls, row.Name(), tag);
}

bool NameWatchSettled(const facts::Function& row, int tag) {
    const wchar_t* cls = nullptr;
    return ScopeOf(row, cls) && ScopedWatchSettled(cls, row.Name(), tag);
}

int PendingNameCount() { return g_namesPending.load(std::memory_order_acquire); }

void DebugForEachWatch(WatchVisitor visit, void* ctx) {
    if (!visit) return;
    for (const Entry& e : g_fnTable)
        if (e.key.load(std::memory_order_acquire) && e.enabled.load(std::memory_order_relaxed))
            visit(ctx, reinterpret_cast<void*>(e.key.load(std::memory_order_relaxed)), nullptr, nullptr, e.tag);
    for (const Entry& e : g_nameTable)
        if (e.key.load(std::memory_order_acquire) && e.enabled.load(std::memory_order_relaxed) && e.name &&
            !e.dead.load(std::memory_order_relaxed))
            visit(ctx, nullptr, e.className, e.name, e.tag);
    // The pending ones too, copied out so the visitor runs outside the registration mutex. Game thread.
    static PendingWatch s_copy[kMaxPending];
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(g_regMutex);
        for (const PendingWatch& r : g_pending)
            if (r.used && r.enabled && !r.dead) s_copy[n++] = r;
    }
    for (int i = 0; i < n; ++i) visit(ctx, nullptr, s_copy[i].className, s_copy[i].name, s_copy[i].tag);
}

LoadStats GetLoadStats() {
    std::lock_guard<std::mutex> lk(g_regMutex);
    LoadStats st{};
    st.nameKeyed = g_nameKeyed;
    st.fnKeyed = g_fnKeyed;
    st.keyedCapacity = kMaxKeyed;
    st.pendingRecords = PendingUsed();
    st.pendingCapacity = kMaxPending;
    return st;
}

namespace {
// What a class-scoped watch's class says of its name once it is loaded: a body it declares, which the loop runs;
// none, or a native one, which never runs through the loop, so the watch can never fire; or nothing yet, the class
// not loaded or still loading. Game thread (the object index answers there).
enum class ClassAnswer : uint8_t { NotLoaded, Declared, Undeclared, Native };

ClassAnswer AskClass(const wchar_t* className, const wchar_t* name) {
    void* cls = ue_wrap::object_index::ClassByName(className);
    if (!cls) return ClassAnswer::NotLoaded;
    void* fn = R::FindFunction(cls, name);
    if (!fn) return ClassAnswer::Undeclared;
    return (Read<std::uint32_t>(fn, P::off::UFunction_FunctionFlags) & P::off::FUNC_Native) ? ClassAnswer::Native
                                                                                          : ClassAnswer::Declared;
}

void LogNeverFires(const wchar_t* className, const wchar_t* name, ClassAnswer a) {
    if (a == ClassAnswer::Native)
        UE_LOGE("script_gate: class '%ls' declares '%ls' native, and a native body never runs through the script "
                "loop -- the watch can never fire", className, name);
    else
        UE_LOGE("script_gate: class '%ls' declares no function '%ls' -- the watch can never fire", className, name);
}

// The class-scoped watches whose class was not loaded when they resolved, asked when the class set next moves (a
// class gains its first instance or loses its last), so a watch registered before its class loads is judged all
// the same: a class that declares no such body kills it, as the resolve would have. Asking a class dispatches
// nothing, so it runs under the registration mutex. One load and a compare while none waits or the set has not
// moved. Game thread.
void JudgeWaitingClasses() {
    if (g_unjudged.load(std::memory_order_relaxed) == 0) return;
    const std::uint64_t set = ue_wrap::object_index::ClassSetVersion();
    if (set == g_judgedAtClassSet) return;
    g_judgedAtClassSet = set;
    int declared = 0, dead = 0;
    {
        std::lock_guard<std::mutex> lk(g_regMutex);
        for (Entry& e : g_nameTable) {
            if (e.key.load(std::memory_order_relaxed) == 0 || !e.unjudged.load(std::memory_order_relaxed)) continue;
            const ClassAnswer a = AskClass(e.className, e.name);
            if (a == ClassAnswer::NotLoaded) continue;
            e.unjudged.store(false, std::memory_order_relaxed);
            g_unjudged.fetch_sub(1, std::memory_order_relaxed);
            if (a == ClassAnswer::Declared) {
                ++declared;
                continue;
            }
            if (e.enabled.exchange(false, std::memory_order_release)) g_nameWatches.fetch_sub(1, std::memory_order_release);
            e.dead.store(true, std::memory_order_release);
            ++dead;
            LogNeverFires(e.className, e.name, a);
        }
        if (declared || dead)
            UE_LOGI("script_gate: %d class-scoped watch(es) judged as their classes loaded: %d declared by their class, "
                    "%d dead; %d still wait", declared + dead, declared, dead, g_unjudged.load(std::memory_order_relaxed));
    }
    if (dead) LogSettleSummary();
}
}  // namespace

void ResolvePendingNames() {
    if (!GT::IsGameThread()) return;
    JudgeWaitingClasses();
    if (g_namesPending.load(std::memory_order_acquire) == 0) return;
    // The string-to-name conversion dispatches ProcessEvent, so it runs OUTSIDE the registration
    // mutex: a registration reached from inside that dispatch would otherwise wait on itself.
    // Under the mutex only the waiting records' literals are collected, and the move is done after.
    // A record is freed only here, on the game thread; the index is checked again under the lock all
    // the same, since a registration reached from that dispatch can take a free record.
    struct Snap { int idx; const wchar_t* name; const wchar_t* className; };
    static Snap snaps[kMaxPending];
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(g_regMutex);
        for (int i = 0; i < kMaxPending; ++i) {
            const PendingWatch& r = g_pending[i];
            if (r.used && !r.dead) snaps[n++] = {i, r.name, r.className};
        }
    }
    int moved = 0, died = 0;
    for (int j = 0; j < n; ++j) {
        const Snap& p = snaps[j];
        const R::FName f = ue_wrap::fname_utils::StringToFName(p.name);
        if (f.ComparisonIndex == 0) continue;   // not yet; the next tick retries
        // A class-scoped watch goes live only with its class's name resolved too.
        R::FName c{};
        if (p.className) {
            c = ue_wrap::fname_utils::StringToFName(p.className);
            if (c.ComparisonIndex == 0) continue;
        }
        // A class-scoped watch fires only for a body its class declares (OtherOwner), and the conversion
        // above adds any name it is given, so a misspelt or mis-copied one resolves all the same: its class
        // is asked now if it is loaded, else when the class set moves (JudgeWaitingClasses).
        const ClassAnswer answer = p.className ? AskClass(p.className, p.name) : ClassAnswer::Declared;
        const bool neverFires = answer == ClassAnswer::Undeclared || answer == ClassAnswer::Native;
        const bool unjudged = p.className && answer == ClassAnswer::NotLoaded;
        std::lock_guard<std::mutex> lk(g_regMutex);
        PendingWatch& r = g_pending[p.idx];
        if (!r.used || r.dead || r.name != p.name || r.className != p.className) continue;
        const wchar_t* ofClass = p.className ? L" of class " : L"";
        const wchar_t* cls = p.className ? p.className : L"";
        g_namesPending.fetch_sub(1, std::memory_order_release);
        // A dead record keeps its literals and the mark, so a consumer asking whether the watch has
        // settled is told, rather than waiting for one that never comes.
        if (neverFires) {
            LogNeverFires(cls, p.name, answer);
            r.dead = true;
            ++died;
            continue;
        }
        // One retired while it waited moves in too, disabled with its literals: a later watch of it
        // re-enables that slot.
        if (Register(g_nameTable, g_nameWatches, g_nameKeyed, NameKey(f), r.tag, r.pre, r.post, p.name,
                     p.className, p.className ? NameKey(c) : 0, /*resolved=*/true, /*enabled=*/r.enabled,
                     unjudged)) {
            const wchar_t* checked = !p.className ? L""
                                     : unjudged ? L"; its class is not loaded yet, and is asked when it loads"
                                                : L", declared by its class";
            if (r.enabled)
                UE_LOGI("script_gate: name '%ls'%ls%ls resolved (cmp=0x%x number=0x%x) -- the watch is live%ls",
                        p.name, ofClass, cls, f.ComparisonIndex, f.Number, checked);
            else
                UE_LOGI("script_gate: name '%ls'%ls%ls resolved after its watch was retired -- kept for a "
                        "re-watch", p.name, ofClass, cls);
            r = PendingWatch{};
            ++moved;
        } else {
            // Not reachable while registration reserves a slot for every waiting watch; said if it ever is.
            LogNoRoom("the name table, at the resolve,", p.className, p.name);
            r.dead = true;
            ++died;
        }
    }
    if ((moved || died) && g_namesPending.load(std::memory_order_acquire) == 0) LogSettleSummary();
}

}  // namespace ue_wrap::script_gate

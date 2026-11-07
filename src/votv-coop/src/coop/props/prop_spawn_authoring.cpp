// coop/props/prop_spawn_authoring.cpp -- see coop/props/prop_spawn_authoring.h for the design.

#include "coop/props/prop_spawn_authoring.h"

#include "coop/element/registry.h"          // EidForActor: an authored actor bound to the wire is shared now
#include "coop/player/hand_item.h"          // LocalHandActor: the player's own hand item
#include "coop/player/players_registry.h"   // Local: the player's own pawn, never a puppet
#include "coop/props/prop_act_request.h"    // ReplayRunning: a granted act's replay
#include "coop/props/prop_echo_suppress.h"  // IsMirrorSpawn
#include "coop/props/shrimp_tray_sync.h"

#include "ue_wrap/core/fname_utils.h"     // StringToFName -- the two verb names, once
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"   // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"     // ClassByName: the widget base
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/script_stack.h"     // the frames and dispatches above the seam
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/ufunction_hook.h"   // CurrentCallerFrame -- the calling Blueprint frame
#include "ue_wrap/facts/function_row.h"

#include <chrono>
#include <cstdint>
#include <cwchar>
#include <string>

namespace coop::prop_spawn_authoring {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace sg = ue_wrap::script_gate;
namespace SS = ue_wrap::script_stack;
namespace UH = ue_wrap::ufunction_hook;

UE_FACT_FN(kRowSpawnPropThroughGamemode, "prop_spawn_authoring", L"mainGamemode_C",
           L"spawnPropThroughGamemode", Declared);

// The gate watch's tag. One consumer, one tag.
constexpr int kGateTag = 0x53504157;  // 'SPAW'

// Resolved once each, on the game thread. The two spawn verbs and the menu's own caller.
void* g_gamemodeSpawnFn = nullptr;   // mainGamemode_C::spawnPropThroughGamemode -- watched
void* g_menuSpawnFn     = nullptr;   // ui_spawnmenu_C::spawn -- the caller that makes it a player's
void* g_toolUberFn      = nullptr;   // tool_spawn_C::ExecuteUbergraph_tool_spawn -- the toolgun
void* g_shovelUberFn    = nullptr;   // prop_shovel_C::ExecuteUbergraph_prop_shovel -- the dirt a shovel throws
bool  g_watchInstalled  = false;

// The negative arm, and it is the half a run cannot otherwise see. Catching every menu spawn and
// rejecting every other caller of the same verb are two different claims, and only the second says
// the world's own spawns still stay local. So each side is counted and both are reported at
// teardown: a run whose `other` column is zero has not exercised the rejection, which is a fact
// about the run rather than about the filter.
unsigned g_verbFromMenu  = 0;   // spawnPropThroughGamemode called BY ui_spawnmenu_C::spawn
unsigned g_verbFromOther = 0;   // ... by anything else (lib_C::replaceProp, comp_physicsImpact, ...)

// The rule tallies the teardown verdict reports: births each rule named the player's at the seam, and
// the ones the drain sent on as intents.
constexpr size_t kRules = static_cast<size_t>(Rule::Count);
unsigned g_authored[kRules] = {};
unsigned g_admitted[kRules] = {};
unsigned g_transientDeaths  = 0;

// The two names the lazy resolves match on, as FNames, so a per-birth test is two int compares
// and never a string build. Zeroed ComparisonIndex means "not yet converted".
R::FName g_menuSpawnName{0, 0};   // "spawn"
R::FName g_toolUberName{0, 0};    // "ExecuteUbergraph_tool_spawn"
R::FName g_shovelUberName{0, 0};  // "ExecuteUbergraph_prop_shovel"

// Is `fn` a function named `name` whose owner (a UFunction's Outer is its declaring class) is
// called `className`? Both compares are allocation-free: the FName pair is two ints, and the
// owner's name goes through the scratch-buffer compare rather than a std::wstring, which matters
// because the FName test does not always short-circuit -- a stranger class could declare a
// function called `spawn` too.
bool FunctionIs(void* fn, const R::FName& name, const wchar_t* className) {
    if (!fn || name.ComparisonIndex == 0) return false;
    const R::FName& fname = R::NameOf(fn);
    if (fname.ComparisonIndex != name.ComparisonIndex || fname.Number != name.Number) return false;
    void* owner = R::OuterOf(fn);
    return owner && R::NameEquals(R::NameOf(owner), className);
}

// The gate's pre. It does NOT open a bracket: the gate's own RAII scope around the body is the
// bracket, and `BirthAuthorship` reads it. This callback only resolves the menu's spawn()
// pointer the first time the menu is the caller -- the widget class is loaded by definition there,
// since its bytecode is what is running -- and counts the two columns the run is judged on.
sg::Verdict OnGamemodeSpawnPre(const sg::Call& call) {
    if (!g_menuSpawnFn && FunctionIs(call.callerFunction, g_menuSpawnName, L"ui_spawnmenu_C"))
        g_menuSpawnFn = call.callerFunction;
    if (call.callerFunction && call.callerFunction == g_menuSpawnFn) {
        ++g_verbFromMenu;
    } else {
        ++g_verbFromOther;
        // Name each refused caller once, so a run says WHICH of the world's own spawn paths it
        // exercised rather than only that it exercised one. A fixed ring, not one slot: two
        // callers alternating past a single slot would render and log on every call.
        static void* s_said[4] = {};
        static int   s_next = 0;
        void* c = call.callerFunction;
        if (c) {
            bool seen = false;
            for (void* p : s_said) if (p == c) { seen = true; break; }
            if (!seen) {
                s_said[s_next] = c;
                s_next = (s_next + 1) % 4;
                UE_LOGI("prop_spawn_authoring: spawnPropThroughGamemode called by '%ls' -- not the "
                        "spawn menu, so the birth is judged by the walk",
                        R::ToString(R::NameOf(c)).c_str());
            }
        }
    }
    return sg::Verdict::Run;                    // observation only; the verb always runs
}

// ---- the verdicts the walk asks of a function and a class, cached by pointer --------------------------

// A UFunction's kind, read from its name once: one of the player's action events or verbs, a tick, or
// anything else. An open-addressed table keyed by the pointer; a full one classifies without keeping.
enum FnKind : uint8_t { kFnOther = 1, kFnAction = 2, kFnTick = 3 };
struct FnSlot {
    void*   fn;
    uint8_t kind;
};
constexpr size_t kFnSlots = 512;   // a power of two
FnSlot g_fnKinds[kFnSlots] = {};

uint8_t ClassifyByName(void* fn) {
    const std::wstring name = R::ToString(R::NameOf(fn));
    for (size_t i = 0; i < P::name::kPlayerActionInputPrefixesSize; ++i) {
        const wchar_t* prefix = P::name::kPlayerActionInputPrefixes[i];
        if (::_wcsnicmp(name.c_str(), prefix, ::wcslen(prefix)) == 0) return kFnAction;
    }
    for (size_t i = 0; i < P::name::kPlayerActionVerbsSize; ++i)
        if (::_wcsicmp(name.c_str(), P::name::kPlayerActionVerbs[i]) == 0) return kFnAction;
    if (::_wcsicmp(name.c_str(), P::name::WidgetTickFn) == 0 || ::_wcsicmp(name.c_str(), P::name::ReceiveTickFn) == 0)
        return kFnTick;
    return kFnOther;
}

uint8_t FnKindOf(void* fn) {
    if (!fn) return kFnOther;
    const size_t home = (reinterpret_cast<uintptr_t>(fn) >> 4) & (kFnSlots - 1);
    for (size_t i = 0; i < kFnSlots; ++i) {
        FnSlot& s = g_fnKinds[(home + i) & (kFnSlots - 1)];
        if (s.fn == fn) return s.kind;
        if (!s.fn) {
            s.kind = ClassifyByName(fn);
            s.fn = fn;
            return s.kind;
        }
    }
    return ClassifyByName(fn);
}

// Whether a class is a widget's (UUserWidget or below), per class pointer.
struct ClassSlot {
    void* cls;
    bool  widget;
};
constexpr int kClassSlots = 64;
ClassSlot g_classKinds[kClassSlots] = {};
int       g_classCount = 0;
void*     g_userWidgetCls = nullptr;   // a native class: resolved once, kept for the process

bool IsWidgetClass(void* cls) {
    if (!cls) return false;
    for (int i = 0; i < g_classCount; ++i)
        if (g_classKinds[i].cls == cls) return g_classKinds[i].widget;
    if (!g_userWidgetCls) g_userWidgetCls = ue_wrap::object_index::ClassByName(P::name::UserWidgetClass);
    if (!g_userWidgetCls) return false;
    const bool widget = R::IsDescendantOfAny(cls, &g_userWidgetCls, 1, 24);
    if (g_classCount < kClassSlots) g_classKinds[g_classCount++] = ClassSlot{cls, widget};
    return widget;
}

// ---- the authored actors: the inheritance memory -------------------------------------------------------

struct Remembered {
    void*    actor;
    int32_t  idx;
    int32_t  serial;
    uint64_t atMs;
};
constexpr size_t   kRemembered = 128;
constexpr uint64_t kInheritMs  = 30000;   // a follow-up birth comes within seconds; past this it is the world's
Remembered g_remembered[kRemembered] = {};
size_t     g_rememberNext = 0;
uint64_t   g_lastRememberedMs = 0;   // the newest entry's time: past kInheritMs no entry can still pass

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// The live entry for `obj`: the same pointer, in the same slot, under the same serial.
const Remembered* FindRemembered(void* obj) {
    if (!obj) return nullptr;
    for (const Remembered& r : g_remembered) {
        if (r.actor != obj || r.idx < 0) continue;
        if (R::ObjectAt(r.idx) == obj && R::SlotSerial(r.idx) == r.serial) return &r;
    }
    return nullptr;
}

// Not bound to a wire identity since its birth: a mirror, or an actor an element row names, is the
// shared world's now, and so is whatever it makes.
bool StillOwnOnly(void* obj) {
    return !coop::prop_echo_suppress::IsMirrorSpawn(obj) &&
           coop::element::Registry::Get().EidForActor(obj) == coop::element::kInvalidId;
}

bool InheritsFrom(void* obj, uint64_t now) {
    const Remembered* r = FindRemembered(obj);
    return r && now - r->atMs <= kInheritMs && StillOwnOnly(obj);
}

// ---- the walk ------------------------------------------------------------------------------------------

// The frames above the seam, innermost first, then the dispatches that started them. Each entry is asked
// three things: is it the player's hand item (or a part of it), one of the player's own action events or
// verbs on the player's own pawn, or an actor an authored birth made a moment ago. Then the outermost
// dispatch alone: a widget's event that is not its tick.
Rule WalkForAuthor(const UH::CallerFrame& frame) {
    SS::Entry chain[SS::kMaxFrames + SS::kMaxDispatches];
    int n = frame.stack ? SS::WalkFrames(frame.stack, chain, SS::kMaxFrames) : 0;
    n += SS::OpenDispatches(chain + n, SS::kMaxDispatches);
    void* const hand = coop::hand_item::LocalHandActor();
    void* const pawn = coop::players::Registry::Get().Local();
    const uint64_t now = NowMs();
    const bool inherit = g_lastRememberedMs != 0 && now - g_lastRememberedMs <= kInheritMs;
    for (int i = 0; i < n; ++i) {
        void* const obj = chain[i].object;
        if (!obj) continue;
        void* const outer = R::OuterOf(obj);
        if (hand && (obj == hand || outer == hand)) return Rule::HandItem;
        if (pawn && obj == pawn && FnKindOf(chain[i].function) == kFnAction) return Rule::PawnAction;
        if (inherit && (InheritsFrom(obj, now) || (outer && InheritsFrom(outer, now)))) return Rule::Inherited;
    }
    const SS::Entry outermost = SS::OutermostDispatch();
    if (outermost.object && IsWidgetClass(R::ClassOf(outermost.object)) &&
        FnKindOf(outermost.function) != kFnTick)
        return Rule::Widget;
    return Rule::None;
}

// Each rule names each class it authors once, so a run says which births each rule carried.
constexpr int kSaidPerRule = 32;
void* g_said[kRules][kSaidPerRule] = {};

void SayOnce(void* actor, Rule rule, const char* what) {
    void* const cls = actor ? R::ClassOf(actor) : nullptr;
    if (!cls) return;
    void** said = g_said[static_cast<size_t>(rule)];
    for (int i = 0; i < kSaidPerRule; ++i) {
        if (said[i] == cls) return;
        if (!said[i]) {
            said[i] = cls;
            UE_LOGI("prop_spawn_authoring: %s %s birth cls=%ls", RuleName(rule), what, R::ClassNameOf(actor).c_str());
            return;
        }
    }
}

// The dispatch each recent birth was made in, both roles.
struct BirthStamp {
    void*    actor;
    uint64_t token;
};
constexpr size_t kBirthStamps = 32;
BirthStamp g_births[kBirthStamps] = {};
size_t     g_birthNext = 0;

// The dispatches a client sent a shared destroy from.
// One entry per destroy, with where the destroyed prop stood: a replacement is born where its original was.
struct DestroyStamp {
    uint64_t token;
    float    at[3];
    bool     hasAt;
};
constexpr size_t kDestroyTokens = 32;
DestroyStamp g_destroyTokens[kDestroyTokens] = {};
size_t       g_destroyNext = 0;

}  // namespace

const char* RuleName(Rule rule) {
    switch (rule) {
        case Rule::Menu:       return "menu";
        case Rule::ShrimpTray: return "shrimp-tray";
        case Rule::Toolgun:    return "toolgun";
        case Rule::Shovel:     return "shovel";
        case Rule::Replay:     return "replay";
        case Rule::HandItem:   return "hand-item";
        case Rule::PawnAction: return "pawn-action";
        case Rule::Widget:     return "widget";
        case Rule::Inherited:  return "inherited";
        case Rule::Replace:    return "replace";
        case Rule::None:
        case Rule::Count:      break;
    }
    return "none";
}

void Install(bool isClient) {
    UE_ASSERT_GAME_THREAD("prop_spawn_authoring::Install");
    // Only a CLIENT asks the question. The host's own births -- a menu spawn, a mushroom spawner,
    // an impact -- are all the shared world by definition, and its spawn watcher broadcasts them
    // without caring who asked, so watching the verb there would buy nothing and cost a lookup on
    // every call of it.
    if (!isClient) return;
    if (g_watchInstalled) return;

    // Throttle the class walks while the gamemode is unresolved (it loads on gameplay entry), the
    // shape prop_drop_intent::Install uses for the same reason.
    static int s_retry = 0;
    if (s_retry > 0) { --s_retry; return; }

    // The two literals as FNames. The conversion dispatches ProcessEvent, so it is game-thread
    // work done once here rather than per birth at the seam.
    if (g_menuSpawnName.ComparisonIndex == 0)
        g_menuSpawnName = ue_wrap::fname_utils::StringToFName(L"spawn");
    if (g_toolUberName.ComparisonIndex == 0)
        g_toolUberName = ue_wrap::fname_utils::StringToFName(L"ExecuteUbergraph_tool_spawn");
    if (g_shovelUberName.ComparisonIndex == 0)
        g_shovelUberName = ue_wrap::fname_utils::StringToFName(L"ExecuteUbergraph_prop_shovel");

    ue_wrap::facts::Resolved gamemodeSpawn;
    if (!g_gamemodeSpawnFn) {
        void* gmCls = R::FindClass(L"mainGamemode_C");
        if (!gmCls) { s_retry = 60; return; }
        gamemodeSpawn = ue_wrap::facts::Resolve(kRowSpawnPropThroughGamemode);
        g_gamemodeSpawnFn = gamemodeSpawn.Fn();
        if (!g_gamemodeSpawnFn) {
            UE_LOGW("prop_spawn_authoring: mainGamemode_C::spawnPropThroughGamemode not found -- a "
                    "client's spawn-menu birth is judged by the walk alone");
            g_watchInstalled = true;   // permanent give-up; do not re-walk every tick
            return;
        }
    }
    if (sg::Watch(gamemodeSpawn, kGateTag, &OnGamemodeSpawnPre, nullptr)) {
        UE_LOGI("prop_spawn_authoring: watching mainGamemode_C::spawnPropThroughGamemode -- a "
                "spawn-menu birth is a player's, a replaceProp or impact birth is the world's");
    } else {
        UE_LOGW("prop_spawn_authoring: the script-gate watch FAILED (gate uninstalled or table "
                "full) -- a client's spawn-menu birth is judged by the walk alone");
    }
    g_watchInstalled = true;
}

Rule BirthAuthorship() {
    // The menu: is the gamemode's spawn verb running on this thread, entered from the menu's own
    // spawn()? The gate answers from the RAII scope it pushes around the body, so the window
    // cannot outlive the body on any exit path, and an inner watched body belonging to some other
    // consumer cannot hide it.
    if (g_menuSpawnFn && sg::IsBodyActive(g_gamemodeSpawnFn, g_menuSpawnFn)) return Rule::Menu;
    // A shrimp tray in this player's own hand, its RMB running: the portion it drops and, at the last one, the empty
    // tray it leaves are the player's (coop/props/shrimp_tray_sync); the tray is its own item, so the host has no copy
    // to run the verb on.
    if (coop::props::shrimp_tray_sync::HandDispenseRunning()) return Rule::ShrimpTray;
    const UH::CallerFrame frame = UH::CurrentCallerFrame();
    if (void* const caller = frame.function) {
        // The toolgun, via the calling frame the native seam already holds: its ubergraph finishes the
        // actor itself. The pointer is cached on first sight -- the class is loaded by definition,
        // since its bytecode is what is running.
        if (caller == g_toolUberFn) return Rule::Toolgun;
        if (!g_toolUberFn && FunctionIs(caller, g_toolUberName, L"tool_spawn_C")) {
            g_toolUberFn = caller;
            UE_LOGI("prop_spawn_authoring: the toolgun's ubergraph is the calling frame -- its births "
                    "are a player's");
            return Rule::Toolgun;
        }
        // The shovel: its ubergraph finishes the dirt ball it throws, a prop the host has no copy of until the thrown
        // ball crosses (coop/props/dig_sync has the hole it digs, which no key names and this door never carries). The
        // same ubergraph finishes the hole; a hole has no key, so that birth leaves the drain at its key wait.
        if (caller == g_shovelUberFn) return Rule::Shovel;
        if (!g_shovelUberFn && FunctionIs(caller, g_shovelUberName, L"prop_shovel_C")) {
            g_shovelUberFn = caller;
            UE_LOGI("prop_spawn_authoring: the shovel's ubergraph is the calling frame -- the dirt it throws is a "
                    "player's");
            return Rule::Shovel;
        }
    }
    // A granted take or use replayed here: our own reflected call, so no player frame stands above it, and
    // what it makes (an opened meal's contents) exists on this peer alone.
    if (coop::prop_act_request::ReplayRunning()) return Rule::Replay;
    // Any other call of ours -- a wire apply, a drill -- is never a player's, even when the pump that runs
    // it drains inside a player's dispatch.
    if (R::InCoopDispatch()) return Rule::None;
    if (!frame.stack) return Rule::None;   // not inside a native seam's callback: nothing to walk
    return WalkForAuthor(frame);
}

bool BirthIsPlayerAuthored() { return BirthAuthorship() != Rule::None; }

void NoteAuthoredBirth(void* actor, Rule rule) {
    UE_ASSERT_GAME_THREAD("prop_spawn_authoring::NoteAuthoredBirth");
    if (!actor || rule == Rule::None || rule == Rule::Count) return;
    ++g_authored[static_cast<size_t>(rule)];
    SayOnce(actor, rule, "authored");
    const int32_t idx = R::InternalIndexOf(actor);
    if (idx < 0) return;
    g_lastRememberedMs = NowMs();
    g_remembered[g_rememberNext] = Remembered{actor, idx, R::AllocateSlotSerial(idx), g_lastRememberedMs};
    g_rememberNext = (g_rememberNext + 1) % kRemembered;
}

bool IsAuthoredActor(void* actor) {
    UE_ASSERT_GAME_THREAD("prop_spawn_authoring::IsAuthoredActor");
    return FindRemembered(actor) && StillOwnOnly(actor);
}

uint64_t DispatchToken() { return SS::OutermostDispatchSerial(); }

void NoteBirthDispatch(void* actor) {
    const uint64_t token = SS::OutermostDispatchSerial();
    if (!actor || token == 0) return;
    g_births[g_birthNext] = BirthStamp{actor, token};
    g_birthNext = (g_birthNext + 1) % kBirthStamps;
}

bool DiesInItsBirthDispatch(void* actor) {
    // Pointer identity only: within one dispatch no collection runs, so the address cannot have been reused.
    const uint64_t token = SS::OutermostDispatchSerial();
    if (!actor || token == 0) return false;
    for (const BirthStamp& b : g_births)
        if (b.actor == actor && b.token == token) return true;
    return false;
}

void NoteTransientDeath() { ++g_transientDeaths; }

void NoteSharedDestroy(const float* at) {
    const uint64_t token = SS::OutermostDispatchSerial();
    if (token == 0) return;
    DestroyStamp& d = g_destroyTokens[g_destroyNext];
    d.token = token;
    d.hasAt = at != nullptr;
    for (int i = 0; i < 3; ++i) d.at[i] = at ? at[i] : 0.f;
    g_destroyNext = (g_destroyNext + 1) % kDestroyTokens;
}

bool SharedDestroyCrossedIn(uint64_t token, const float* birthAt, float radiusCm) {
    if (token == 0) return false;
    for (const DestroyStamp& d : g_destroyTokens) {
        if (d.token != token) continue;
        // A destroy whose place was unreadable pairs by dispatch alone, as before.
        if (!d.hasAt || !birthAt) return true;
        const float dx = d.at[0] - birthAt[0], dy = d.at[1] - birthAt[1], dz = d.at[2] - birthAt[2];
        if (dx * dx + dy * dy + dz * dz <= radiusCm * radiusCm) return true;
    }
    return false;
}

void NoteAdmitted(void* actor, Rule rule) {
    if (rule == Rule::None || rule == Rule::Count) return;
    ++g_admitted[static_cast<size_t>(rule)];
    if (rule == Rule::Replace) SayOnce(actor, rule, "paired");
}

void Reset() {
    UE_ASSERT_GAME_THREAD("prop_spawn_authoring::Reset");
    if (g_watchInstalled)
        UE_LOGI("prop_spawn_authoring: VERDICT spawnPropThroughGamemode calls: from-the-menu=%u "
                "(crossed as intents) from-elsewhere=%u (judged by the walk)%s",
                g_verbFromMenu, g_verbFromOther,
                g_verbFromOther == 0 ? " -- this run never exercised the rejection" : "");
    unsigned anyAuthored = 0;
    for (const unsigned c : g_authored) anyAuthored += c;
    if (anyAuthored != 0 || g_admitted[static_cast<size_t>(Rule::Replace)] != 0 || g_transientDeaths != 0) {
        const auto at = [](const unsigned (&a)[kRules], Rule r) { return a[static_cast<size_t>(r)]; };
        UE_LOGI("prop_spawn_authoring: VERDICT births authored/crossed by rule: menu=%u/%u shrimp-tray=%u/%u "
                "toolgun=%u/%u shovel=%u/%u replay=%u/%u hand-item=%u/%u pawn-action=%u/%u widget=%u/%u "
                "inherited=%u/%u replace=-/%u; transient deaths kept off the wire=%u",
                at(g_authored, Rule::Menu), at(g_admitted, Rule::Menu),
                at(g_authored, Rule::ShrimpTray), at(g_admitted, Rule::ShrimpTray),
                at(g_authored, Rule::Toolgun), at(g_admitted, Rule::Toolgun),
                at(g_authored, Rule::Shovel), at(g_admitted, Rule::Shovel),
                at(g_authored, Rule::Replay), at(g_admitted, Rule::Replay),
                at(g_authored, Rule::HandItem), at(g_admitted, Rule::HandItem),
                at(g_authored, Rule::PawnAction), at(g_admitted, Rule::PawnAction),
                at(g_authored, Rule::Widget), at(g_admitted, Rule::Widget),
                at(g_authored, Rule::Inherited), at(g_admitted, Rule::Inherited),
                at(g_admitted, Rule::Replace), g_transientDeaths);
    }
    g_verbFromMenu = 0;
    g_verbFromOther = 0;
    for (unsigned& c : g_authored) c = 0;
    for (unsigned& c : g_admitted) c = 0;
    g_transientDeaths = 0;
    // The watch and the resolved UFunctions are NOT per session: they name the game's own classes, which
    // outlive any session in this process, and the gate is already session-gated. What this session's
    // world made -- the remembered actors, the dispatch stamps -- goes, and so do the cached verdicts,
    // whose pointers a world reload can hand to other functions and classes.
    for (Remembered& r : g_remembered) r = Remembered{};
    g_rememberNext = 0;
    g_lastRememberedMs = 0;
    for (BirthStamp& b : g_births) b = BirthStamp{};
    g_birthNext = 0;
    for (DestroyStamp& d : g_destroyTokens) d = DestroyStamp{};
    g_destroyNext = 0;
    for (FnSlot& s : g_fnKinds) s = FnSlot{};
    g_classCount = 0;
    for (auto& row : g_said)
        for (void*& p : row) p = nullptr;
}

}  // namespace coop::prop_spawn_authoring

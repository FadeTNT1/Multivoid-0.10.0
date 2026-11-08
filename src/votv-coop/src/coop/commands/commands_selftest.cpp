// coop/commands/commands_selftest.cpp -- the un-gated boot selftest of coop/commands/, run once
// per session start on both peers (shape: coop/net/stream_slot_selftest.cpp). A resolver that picks
// the wrong player kicks the wrong player, and a wrong pick reads as working until it does.
//
// Red arm: with the dev row selftest_break_commands the FIRST case expects the wrong word count,
// so the run must fail.

#include "coop/commands/commands_selftest.h"

#include "coop/commands/command_dispatcher.h"
#include "coop/commands/command_line.h"
#include "coop/commands/command_registry.h"
#include "coop/commands/command_targets.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace coop::commands {

namespace {

// UTF-8 spelled out as bytes: the file's own encoding is not what is under test.
const char kLQuote[] = "\xE2\x80\x9C";                  // U+201C
const char kRQuote[] = "\xE2\x80\x9D";                  // U+201D
const char kIvanUpper[] = "\xD0\x98\xD0\xB2\xD0\xB0\xD0\xBD";  // U+0418 U+0432 U+0430 U+043D
const char kIvanLower[] = "\xD0\xB8\xD0\xB2\xD0\xB0\xD0\xBD";  // U+0438 U+0432 U+0430 U+043D

bool SameWords(const ParsedLine& p, std::initializer_list<const char*> want) {
    if (p.words.size() != want.size() || p.begins.size() != want.size()) return false;
    size_t i = 0;
    for (const char* w : want)
        if (p.words[i++] != w) return false;
    return true;
}

bool SameBegins(const ParsedLine& p, std::initializer_list<size_t> want) {
    if (p.begins.size() != want.size()) return false;
    size_t i = 0;
    for (size_t b : want)
        if (p.begins[i++] != b) return false;
    return true;
}

PlayerView MakePlayer(int slot, unsigned no, const char* nick, const std::string& id,
                      bool hasPos, float x) {
    PlayerView p;
    p.slot = slot;
    p.playerNo = no;
    p.nick = nick;
    p.playerId = id;
    p.hasPosition = hasPos;
    p.x = x;
    return p;
}

std::string Repeat(const char* s, int times) {
    std::string out;
    for (int i = 0; i < times; ++i) out += s;
    return out;
}

std::vector<PlayerView> FourPlayers() {
    return {MakePlayer(0, 1, "Host", Repeat("a", 32), true, 0),
            MakePlayer(1, 2, "Bob", Repeat("b1", 16), true, 100),
            MakePlayer(2, 5, "bobby", Repeat("b2", 16), true, 50),
            MakePlayer(3, 4, kIvanUpper, Repeat("c", 32), false, 0)};
}

int PickOne(int) { return 1; }
int PickNine(int) { return 9; }

bool Resolves(const TargetResult& r, std::initializer_list<int> slots) {
    if (r.error != TargetError::None || r.slots.size() != slots.size() ||
        r.playerIds.size() != slots.size())
        return false;
    size_t i = 0;
    for (int s : slots)
        if (r.slots[i++] != s) return false;
    return true;
}

bool Matches(const TargetResult& r, std::initializer_list<int> slots) {
    if (r.error != TargetError::Ambiguous || r.matches.size() != slots.size()) return false;
    size_t i = 0;
    for (int s : slots)
        if (r.matches[i++] != s) return false;
    return true;
}

void SplitCases(Checker& check, bool breakIt) {
    {
        const ParsedLine p = SplitLine("kick Bob");
        const size_t expectedWords = breakIt ? 3 : 2;
        check(p.words.size() == expectedWords && SameWords(p, {"kick", "Bob"}) &&
                  SameBegins(p, {0, 5}),
              "split: kick Bob is two words at 0 and 5");
    }
    check(SameWords(SplitLine("  kick   Bob  "), {"kick", "Bob"}),
          "split: a run of spaces separates once, the ends make no word");
    {
        const ParsedLine p = SplitLine("ban \"Bob Smith\" griefing");
        check(SameWords(p, {"ban", "Bob Smith", "griefing"}) && SameBegins(p, {0, 4, 16}),
              "split: a quoted word keeps its spaces; begins is the opening quote");
    }
    check(SameWords(SplitLine("say \"unclosed text"), {"say", "unclosed text"}),
          "split: an unclosed quote runs to the end");
    check(SameWords(SplitLine("a\"b c"), {"a\"b", "c"}), "split: a quote inside a word is a byte");
    check(SameWords(SplitLine("x \"\""), {"x", ""}), "split: an empty quoted word is a word");
    check(SameWords(SplitLine("\"a b\"c"), {"a b", "c"}), "split: a closing quote ends its word");
    {
        const std::string curly = std::string("msg Bob ") + kLQuote + "hi there" + kRQuote;
        check(SameWords(SplitLine(curly), {"msg", "Bob", "hi there"}),
              "split: U+201C and U+201D quote like \"");
    }
    check(SplitLine("").words.empty() && SplitLine("   ").words.empty(),
          "split: nothing makes no words");
}

void TargetCases(Checker& check) {
    const std::vector<PlayerView> players = FourPlayers();
    Caller host;
    host.slot = 0;
    host.isOperator = true;
    Caller console;
    Caller slot3;
    slot3.slot = 3;
    auto resolve = [&](const char* word, bool one = true, const Caller& who = Caller{0, 0, true},
                       int (*pick)(int) = nullptr) {
        return ResolveTarget(word, one, false, who, players, pick);
    };

    check(resolve("").error == TargetError::NoMatch, "target: an empty word names nobody");
    check(Resolves(resolve("@s"), {0}), "target: @s is the caller");
    {
        const TargetResult r = resolve("@a", false);
        check(Resolves(r, {0, 1, 2, 3}) && r.usedSelector, "target: @a is everyone");
    }
    check(resolve("@a", true).error == TargetError::TooMany, "target: @a for one player refuses");
    check(Resolves(resolve("@p"), {2}), "target: @p is the nearest other player");
    check(resolve("@p", true, console).error == TargetError::NoCaller,
          "target: @p by the console needs a caller");
    check(resolve("@p", true, slot3).error == TargetError::NoPosition,
          "target: @p from a caller without a position");
    {
        const TargetResult r = resolve("@r", true, host, PickOne);
        check(Resolves(r, {1}) && r.usedSelector, "target: @r takes pick's index");
    }
    check(resolve("@r", true, host, PickNine).error == TargetError::NoMatch,
          "target: @r with a pick outside the range names nobody");
    check(Resolves(resolve("#5"), {2}), "target: #5 is player number 5");
    check(Resolves(resolve("#05"), {2}), "target: leading zeros allowed");
    check(Resolves(resolve("#2"), {1}), "target: #2 is player number 2");
    check(resolve("#7").error == TargetError::NoMatch, "target: #7 names nobody");
    check(resolve("#").error == TargetError::NoMatch, "target: # alone names nobody");
    check(resolve("#0").error == TargetError::NoMatch, "target: #0 names nobody");
    check(resolve("#65537").error == TargetError::NoMatch, "target: #65537 is past 16 bits");
    check(resolve("@x").error == TargetError::NoMatch, "target: an unknown selector names nobody");
    {
        const TargetResult r = resolve("bob");
        check(Resolves(r, {1}) && r.playerIds[0] == Repeat("b1", 16),
              "target: an exact nick beats the substring and carries its id");
    }
    check(Resolves(resolve("BOBBY"), {2}), "target: nicks fold case");
    {
        const TargetResult r = resolve("bo");
        check(Matches(r, {1, 2}), "target: a shared substring is ambiguous, ascending");
        check(DescribeTargetError(r, "bo", players) ==
                  "'bo' matches several players: Bob (#2), bobby (#5).",
              "target: the ambiguous line lists the matches with their # forms");
    }
    check(Resolves(resolve(kIvanLower), {3}), "target: a Cyrillic nick folds");
    check(Resolves(resolve("b2b2b2b2"), {2}), "target: an id prefix");
    check(Resolves(resolve("B2B2B2B2"), {2}), "target: an id prefix is lowered");
    check(resolve("b1b1").error == TargetError::NoMatch, "target: under 8 hex digits is a nick");
    check(resolve("zzz").error == TargetError::NoMatch, "target: a stranger names nobody");

    const std::vector<PlayerView> clash = {
        MakePlayer(0, 1, "b2b2b2b2", Repeat("a", 32), true, 0),
        MakePlayer(1, 2, "x", Repeat("b2", 16), true, 0)};
    check(Resolves(ResolveTarget("b2b2b2b2", true, false, host, clash, nullptr), {1}),
          "target: an id beats an identical nick");
}

// ---- the registry and the dispatcher ----

// What the last handler call saw: Context lives for the call only.
struct Captured {
    bool called = false;
    std::string path;
    std::vector<TargetResult> targets;
    std::vector<long long> integers;
    std::vector<std::string> texts;
    std::vector<int> notifySlots;
};
Captured g_cap;

void Capture(Context& ctx) {
    g_cap.called = true;
    g_cap.path = ctx.registry.PathOf(ctx.spec);
    g_cap.targets = ctx.targets;
    g_cap.integers = ctx.integers;
    g_cap.texts = ctx.texts;
    g_cap.notifySlots = ctx.notifySlots;
    ctx.Reply("ok");
}

bool CheckDefault(const Caller&, std::string_view, bool defaultGranted) { return defaultGranted; }
bool CheckAll(const Caller&, std::string_view, bool) { return true; }
bool CheckAllButSelector(const Caller&, std::string_view node, bool) {
    return node != "multivoid.command.selector";
}

CommandSpec KickSpec() {
    CommandSpec c;
    c.name = "kick";
    c.description = "Removes a player from the session.";
    c.args = {{"who", ArgKind::Player, false}, {"reason", ArgKind::Rest, true}};
    c.handler = &Capture;
    return c;
}

CommandSpec TimeSpec(const char* name) {
    CommandSpec set;
    set.name = "set";
    set.description = "Sets the time of day.";
    set.args = {{"value", ArgKind::Word, false}};
    set.handler = &Capture;
    CommandSpec add;
    add.name = "add";
    add.description = "Moves the time of day on.";
    add.nodeOf = "multivoid.time.set";
    add.args = {{"amount", ArgKind::Word, false}};
    add.handler = &Capture;
    CommandSpec t;
    t.name = name;
    t.defaultGranted = true;
    t.description = "Shows the time of day.";
    t.aliases = {{"day", "set day"}};
    t.handler = &Capture;
    t.subVerbs = {set, add};
    return t;
}

// Registers help and the test commands; false if any refused.
bool BuildRegistry(Registry& reg, bool kickAliases) {
    bool ok = RegisterBuiltins(reg);
    CommandSpec kick = KickSpec();
    if (kickAliases) kick.aliases = {{"boot", "Bob"}, {"why", "Bob said"}};
    ok = reg.Register(kick, nullptr) && ok;
    CommandSpec tp;
    tp.name = "tp";
    tp.description = "Teleports players.";
    tp.args = {{"who", ArgKind::Players, false}};
    tp.handler = &Capture;
    ok = reg.Register(tp, nullptr) && ok;
    ok = reg.Register(TimeSpec("time"), nullptr) && ok;
    CommandSpec add;
    add.name = "add";
    add.defaultGranted = true;
    add.description = "Adds a number.";
    add.args = {{"n", ArgKind::Integer, false}};
    add.handler = &Capture;
    ok = reg.Register(add, nullptr) && ok;
    return ok;
}

// A valid spec in kick's shape; each refusal case is a copy with exactly one defect.
CommandSpec Probe() {
    CommandSpec c = KickSpec();
    c.name = "probe";
    return c;
}

bool Refuses(Registry& reg, const CommandSpec& c) {
    std::string why;
    return !reg.Register(c, &why) && !why.empty();
}

// Refused, and the reason names the rule the case is about.
bool RefusesFor(Registry& reg, const CommandSpec& c, const char* reasonPart) {
    std::string why;
    return !reg.Register(c, &why) && why.find(reasonPart) != std::string::npos;
}

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

bool SaidOnly(const DispatchResult& r, const char* line) {
    return !r.ran && r.replies.size() == 1 && r.replies[0] == line;
}

DispatchResult Run(const Registry& reg, const Caller& who, const char* line,
                   const std::vector<PlayerView>& players, const Policy& policy) {
    g_cap = Captured{};
    return Dispatch(reg, who, line, players, policy);
}

void RegistryCases(Checker& check) {
    {
        Registry control;
        check(control.Register(Probe(), nullptr), "registry: the control probe registers");
    }

    Registry reg;
    check(BuildRegistry(reg, false), "registry: help and the test commands register");

    CommandSpec p = Probe();
    p.name = "admin";
    check(Refuses(reg, p), "registry: a reserved root name is refused");
    p = Probe();
    p.name = "Kick!";
    check(Refuses(reg, p), "registry: a name outside [a-z0-9] is refused");
    // The duplicates below name an already-declared node, so the derived-node rule cannot refuse
    // them: only the rule each case names can.
    p = Probe();
    p.name = "kick";
    p.nodeOf = "multivoid.help";
    check(Refuses(reg, p), "registry: a second kick is refused");
    p = Probe();
    p.aliases = {{"tp", ""}};
    check(Refuses(reg, p), "registry: an alias that is another root's name is refused");
    p = Probe();
    p.handler = nullptr;
    p.args.clear(); p.qualifiers.clear();
    CommandSpec set = KickSpec();
    set.name = "set";
    CommandSpec twin = set;
    twin.nodeOf = "multivoid.help";
    p.subVerbs = {twin, twin};
    check(RefusesFor(reg, p, "two sub-verbs named"), "registry: two sub-verbs of one name are refused");
    p = Probe();
    p.args = {{"t", ArgKind::Rest, false}, {"w", ArgKind::Word, false}};
    check(Refuses(reg, p), "registry: a Rest before another argument is refused");
    p = Probe();
    p.args = {{"a", ArgKind::Word, true}, {"b", ArgKind::Word, false}};
    check(Refuses(reg, p), "registry: a required argument after an optional one is refused");
    p = Probe();
    p.handler = nullptr;
    check(Refuses(reg, p), "registry: no handler and no sub-verbs is refused");
    p = Probe();
    p.handler = nullptr;
    p.args.clear(); p.qualifiers.clear();
    set.aliases = {{"s", ""}};
    p.subVerbs = {set};
    check(RefusesFor(reg, p, "an alias on a sub-verb"), "registry: an alias on a sub-verb is refused");
    p = Probe();
    p.nodeOf = "multivoid.nosuch";
    check(Refuses(reg, p), "registry: a nodeOf that is not declared is refused");

    CommandSpec r;
    r.name = "r";
    r.nodeOf = "multivoid.add";
    r.args = {{"text", ArgKind::Rest, false}};
    r.handler = &Capture;
    check(reg.Register(r, nullptr), "registry: a verb naming a declared node registers");
    const CommandSpec* rRoot = reg.FindRoot("r", nullptr);
    check(rRoot != nullptr && reg.NodeOf(*rRoot) == "multivoid.add", "registry: nodeOf is the node");
    CommandSpec clockSpec = TimeSpec("clock");
    clockSpec.aliases = {{"bad", "nosuch"}};
    check(Refuses(reg, clockSpec), "registry: an alias whose preset word names no sub-verb is refused");
    check(reg.DeclareNode({"multivoid.zzz", false, ""}, nullptr), "registry: a node can be declared");
    check(!reg.DeclareNode({"multivoid.zzz", false, ""}, nullptr), "registry: not twice");
    p = Probe();
    p.name = "zzz";
    check(Refuses(reg, p), "registry: a root whose derived node is declared is refused");
    check(reg.FindNode("multivoid.admin") == nullptr && reg.FindNode("multivoid.probe") == nullptr &&
              reg.FindNode("multivoid.clock") == nullptr,
          "registry: nothing of a refused tree was declared");

    const CommandSpec* timeRoot = reg.FindRoot("time", nullptr);
    const CommandSpec* kickRoot = reg.FindRoot("kick", nullptr);
    check(timeRoot != nullptr && reg.NodeOf(timeRoot->subVerbs[0]) == "multivoid.time.set" &&
              reg.NodeOf(timeRoot->subVerbs[1]) == "multivoid.time.set",
          "registry: a sub-verb's node, and nodeOf onto a sibling's");
    check(kickRoot != nullptr && reg.Usage(*kickRoot) == "/kick <who> [reason...]", "registry: kick's usage");
    check(timeRoot != nullptr && reg.Usage(timeRoot->subVerbs[0]) == "/time set <value>",
          "registry: a sub-verb's usage");
    check(timeRoot != nullptr && reg.Usage(*timeRoot) == "/time [set|add]", "registry: sub-verbs in usage");

    const std::vector<PlayerView> players = FourPlayers();
    Caller who;
    who.slot = 1;
    Policy byDefault;
    byDefault.check = &CheckDefault;
    Policy everything;
    everything.check = &CheckAll;
    Policy noSelector;
    noSelector.check = &CheckAllButSelector;

    check(SaidOnly(Run(reg, who, "kick Bob", players, byDefault),
                   "You do not have permission for /kick (multivoid.kick)."),
          "dispatch: a node the caller lacks is refused with its name");
    {
        const DispatchResult h = Run(reg, who, "help", players, byDefault);
        check(h.ran && h.replies.size() == 5 && h.replies[0] == "Commands you can use:" &&
                  StartsWith(h.replies[1], "/add <n> -- ") &&
                  StartsWith(h.replies[2], "/help [command...] -- ") &&
                  StartsWith(h.replies[3], "/r <text...> -- ") &&
                  StartsWith(h.replies[4], "/time [set|add] -- "),
              "dispatch: help lists exactly what the caller may use");
    }
    {
        const DispatchResult h = Run(reg, who, "help kick", players, byDefault);
        check(h.ran && h.replies.size() == 1 && StartsWith(h.replies[0], "/kick <who> [reason...] -- "),
              "dispatch: help of a command shows its usage, permitted or not");
    }
    check(Run(reg, who, "help nosuch", players, byDefault).replies ==
              std::vector<std::string>{"Unknown command '/nosuch'."},
          "dispatch: help of a stranger");
    check(SaidOnly(Run(reg, who, "nosuch", players, byDefault),
                   "Unknown command '/nosuch'. Type /help for the commands."),
          "dispatch: an unknown command");
    check(SaidOnly(Run(reg, who, "", players, byDefault), "Type /help for the commands."),
          "dispatch: an empty line hints at help");
    check(SaidOnly(Run(reg, who, "add x", players, byDefault),
                   "'x' is not a whole number. Usage: /add <n>"),
          "dispatch: a word that is not a whole number");
    {
        const DispatchResult a = Run(reg, who, "add +5", players, byDefault);
        check(a.ran && g_cap.integers.size() == 1 && g_cap.integers[0] == 5, "dispatch: +5 is 5");
    }
    check(SaidOnly(Run(reg, who, "add 5 6", players, byDefault), "Usage: /add <n>"),
          "dispatch: a word left over is a usage error");
    check(SaidOnly(Run(reg, who, "add +-5", players, byDefault),
                   "'+-5' is not a whole number. Usage: /add <n>"),
          "dispatch: +-5 is not a whole number");
    check(Run(reg, who, "time", players, byDefault).ran && g_cap.path == "time",
          "dispatch: a verb with sub-verbs runs its own handler");
    check(SaidOnly(Run(reg, who, "day", players, byDefault),
                   "You do not have permission for /time set (multivoid.time.set)."),
          "dispatch: an alias is checked as its expansion");

    check(SaidOnly(Run(reg, who, "kick bo", players, everything),
                   "'bo' matches several players: Bob (#2), bobby (#5)."),
          "dispatch: an ambiguous target lists its matches");
    {
        const DispatchResult k = Run(reg, who, "kick Bob said \"lol  ", players, everything);
        check(k.ran && g_cap.targets[0].slots == std::vector<int>{1} &&
                  g_cap.texts[1] == "said \"lol",
              "dispatch: Rest keeps the raw remainder, quotes and all");
    }
    {
        const DispatchResult d = Run(reg, who, "DAY", players, everything);
        check(d.ran && g_cap.path == "time set" && g_cap.texts[0] == "day",
              "dispatch: an alias runs its expansion, in any case");
    }
    check(SaidOnly(Run(reg, who, "time set", players, everything), "Usage: /time set <value>"),
          "dispatch: a missing required argument prints the usage");
    check(SaidOnly(Run(reg, who, "tp @a", players, noSelector),
                   "You do not have permission to use @a or @r (multivoid.command.selector)."),
          "dispatch: @a needs the selector node");

    Registry withAliases;
    check(BuildRegistry(withAliases, true), "registry: kick with aliases registers");
    {
        const DispatchResult b = Run(withAliases, who, "boot being rude", players, everything);
        check(b.ran && g_cap.targets[0].slots == std::vector<int>{1} &&
                  g_cap.texts[1] == "being rude",
              "dispatch: Rest after an alias takes the raw typed remainder");
    }
    {
        const DispatchResult w = Run(withAliases, who, "why hi  there", players, everything);
        check(w.ran && g_cap.texts[1] == "said hi  there",
              "dispatch: Rest starting on a preset word joins the preset and the raw remainder");
    }
}

// ---- qualifiers, offline targets, console-only verbs ----

bool NoOfflineNode(const Caller&, std::string_view node, bool) {
    return node.size() < 8 || node.substr(node.size() - 8) != ".offline";
}
bool HoldsAnyone(std::string_view, std::string_view, bool) { return true; }
// The host has a record of the ids of 32 `d` and of 32 `e`.
bool KnownDAndE(std::string_view id) { return id == Repeat("d", 32) || id == Repeat("e", 32); }
// Bob (slot 1) and the id of 32 `e` hold zap.exempt explicitly.
bool ExplicitBobAndE(std::string_view id, std::string_view node) {
    return node == "multivoid.zap.exempt" && (id == Repeat("b1", 16) || id == Repeat("e", 32));
}

CommandSpec ZapSpec() {
    CommandSpec c;
    c.name = "zap";
    c.description = "Zaps a player.";
    c.pastTense = "zapped";
    c.args = {{"who", ArgKind::PlayerOrId, false, true}, {"reason", ArgKind::Rest, true}};
    c.qualifiers = {{"offline", QualKind::GateOffline},
                    {"exempt", QualKind::Exempt},
                    {"notify", QualKind::Notify}};
    c.handler = &Capture;
    return c;
}

void QualifierCases(Checker& check) {
    const std::vector<PlayerView> players = FourPlayers();
    Caller who;
    who.slot = 1;
    Caller console{0, 0, true};
    Policy everything;
    everything.check = &CheckAll;
    Policy full = everything;
    full.holds = &HoldsAnyone;
    full.isExplicit = &ExplicitBobAndE;
    full.known = &KnownDAndE;
    Policy noOffline = full;
    noOffline.check = &NoOfflineNode;
    const std::string offlineId = Repeat("d", 32);

    {
        Registry reg;
        int hits = 0;
        CommandSpec c = Probe();
        c.name = "cap";
        c.handler = [&hits](Context& ctx) {
            ++hits;
            ctx.Reply("captured");
        };
        check(reg.Register(c, nullptr), "qualifier: a capturing handler registers");
        const DispatchResult r = Run(reg, who, "cap Bob", players, everything);
        check(r.ran && hits == 1 && r.replies == std::vector<std::string>{"captured"},
              "qualifier: a handler that captures a local runs and sees it");
    }

    check(!ResolveTarget(offlineId, true, false, console, players, nullptr).offline,
          "qualifier: a 32-hex word is not offline without orId");
    {
        const TargetResult r = ResolveTarget(Repeat("D", 32), true, true, console, players, nullptr);
        check(r.error == TargetError::None && r.offline && r.offlineId == offlineId && r.slots.empty(),
              "qualifier: a PlayerOrId word naming nobody seated but 32 hex is offline, lowered");
    }
    {
        const TargetResult r = ResolveTarget(Repeat("a", 32), true, true, console, players, nullptr);
        check(!r.offline && Resolves(r, {0}), "qualifier: a seated id stays online under orId");
    }
    check(ResolveTarget(Repeat("d", 31), true, true, console, players, nullptr).error ==
              TargetError::NoMatch,
          "qualifier: 31 hex is not an offline id");

    Registry reg;
    check(RegisterBuiltins(reg) && reg.Register(ZapSpec(), nullptr), "qualifier: the zap tree registers");
    const NodeDecl* declared = reg.FindNode("multivoid.zap.exempt");
    check(declared != nullptr && !declared->defaultGranted &&
              declared->description == "Zaps a player. -- exempt" &&
              reg.FindNode("multivoid.zap.offline") != nullptr &&
              reg.FindNode("multivoid.zap.notify") != nullptr,
          "qualifier: each qualifier declares its node, default false");

    check(SaidOnly(Run(reg, who, ("zap " + offlineId).c_str(), players, noOffline),
                   "You do not have permission for /zap on an offline player (multivoid.zap.offline)."),
          "qualifier: an offline target needs the .offline node");
    {
        const DispatchResult r = Run(reg, who, ("zap " + offlineId).c_str(), players, full);
        check(r.ran && g_cap.targets[0].offline && g_cap.targets[0].offlineId == offlineId,
              "qualifier: an offline target runs when the caller passes .offline");
    }
    check(Run(reg, who, "zap bobby", players, noOffline).ran,
          "qualifier: a seated target needs no .offline node");
    {
        const std::string unseenId = Repeat("f", 32);
        const char* unseen = "ffffffff has never played here; only the host can act on an unknown id.";
        check(SaidOnly(Run(reg, who, ("zap " + unseenId).c_str(), players, full), unseen),
              "qualifier: a caller that is not the console is refused an offline id the host never saw");
        check(Run(reg, console, ("zap " + unseenId).c_str(), players, full).ran,
              "qualifier: the console is not stopped by an offline id the host never saw");
        Policy noKnown = full;
        noKnown.known = nullptr;
        check(SaidOnly(Run(reg, who, ("zap " + offlineId).c_str(), players, noKnown),
                       "dddddddd has never played here; only the host can act on an unknown id."),
              "qualifier: a null known treats every offline id as unseen");
        check(Run(reg, who, ("zap " + offlineId).c_str(), players, full).ran,
              "qualifier: a caller that is not the console may act on an offline id the host knows");
        check(SaidOnly(Run(reg, who, ("zap " + unseenId).c_str(), players, noOffline),
                       "You do not have permission for /zap on an offline player (multivoid.zap.offline)."),
              "qualifier: the offline node is asked before the known id");
    }

    check(SaidOnly(Run(reg, who, "zap Bob", players, full), "Bob cannot be zapped."),
          "qualifier: an explicit exempt holder cannot be acted on");
    check(SaidOnly(Run(reg, who, ("zap " + Repeat("e", 32)).c_str(), players, full),
                   "eeeeeeee cannot be zapped."),
          "qualifier: an offline target is judged by its id");
    check(Run(reg, console, "zap Bob", players, full).ran,
          "qualifier: the console is not stopped by .exempt");
    check(SaidOnly(Run(reg, who, "zap Bob", players, everything),
                   "Bob's identity is not proved yet."),
          "qualifier: a policy with no isExplicit refuses an exempt target as unproved");
    {
        std::vector<PlayerView> unproved = players;
        unproved[1].playerId.clear();
        check(SaidOnly(Run(reg, who, "zap #2", unproved, full), "Bob's identity is not proved yet."),
              "qualifier: an online target with an empty playerId is refused under Exempt");
        check(Run(reg, console, "zap #2", unproved, full).ran,
              "qualifier: the console needs no proved id for a target");
    }

    check(SaidOnly(Run(reg, console, "zap Host", players, full),
                   "That is the host -- it cannot be zapped."),
          "qualifier: a notHost target that is slot 0 is refused with the spec's pastTense, even to the console");

    {
        const DispatchResult r = Run(reg, who, "zap bobby", players, full);
        check(r.ran && g_cap.notifySlots == std::vector<int>{2, 3},
              "qualifier: notifySlots excludes the caller and slot 0");
        const DispatchResult s = Run(reg, console, "zap bobby", players, full);
        check(s.ran && g_cap.notifySlots == std::vector<int>{1, 2, 3},
              "qualifier: the console's notifySlots excludes slot 0 only");
        Policy noHolds = full;
        noHolds.holds = nullptr;
        const DispatchResult n = Run(reg, who, "zap bobby", players, noHolds);
        check(n.ran && g_cap.notifySlots.empty(), "qualifier: a null holds notifies nobody");
    }

    {
        CommandSpec c = ZapSpec();
        c.name = "zapped";
        c.pastTense.clear();
        Registry bare;
        c.qualifiers = {{"notify", QualKind::Notify}};
        check(RefusesFor(bare, c, "needs a pastTense"),
              "qualifier: a notHost argument without a pastTense is refused");
        c.args = {{"who", ArgKind::Player, false}};
        c.qualifiers = {{"exempt", QualKind::Exempt}};
        check(RefusesFor(bare, c, "needs a pastTense"),
              "qualifier: an Exempt qualifier without a pastTense is refused");
        c.qualifiers = {{"notify", QualKind::Notify}};
        check(bare.Register(c, nullptr),
              "qualifier: a spec with neither needs no pastTense");
    }

    {
        CommandSpec c = ZapSpec();
        c.name = "zapid";
        c.description = "Zaps a player by id.";
        c.nodeOf = "multivoid.zap";
        check(reg.Register(c, nullptr), "qualifier: a second spec naming the node registers");
        const NodeDecl* again = reg.FindNode("multivoid.zap.exempt");
        check(again == declared && again->description == "Zaps a player. -- exempt" &&
                  reg.FindNode("multivoid.zapid.exempt") == nullptr,
              "qualifier: a qualifier node is declared once for two specs naming one node");
        check(SaidOnly(Run(reg, who, "zapid Bob", players, full), "Bob cannot be zapped."),
              "qualifier: the second spec judges the same exempt node");
    }
    {
        CommandSpec c = ZapSpec();
        c.name = "zapsub";
        CommandSpec sub = Probe();
        sub.name = "notify";
        c.subVerbs = {sub};
        Registry bare;
        check(RefusesFor(bare, c, "already declared"),
              "qualifier: a command node derived onto a qualifier node is refused");
    }

    {
        Registry bare;
        CommandSpec c = ZapSpec();
        c.args = {{"who", ArgKind::Players, false}};
        c.qualifiers = {{"exempt", QualKind::Exempt}};
        check(RefusesFor(bare, c, "needs exactly one Player or PlayerOrId"),
              "qualifier: an Exempt qualifier on a Players target is refused");
        c.args.clear();
        c.qualifiers = {{"offline", QualKind::GateOffline}};
        check(RefusesFor(bare, c, "needs exactly one Player or PlayerOrId"),
              "qualifier: a GateOffline qualifier on a spec with no target is refused");
        c.args = {{"a", ArgKind::Player, false}, {"b", ArgKind::Player, false}};
        c.qualifiers = {{"exempt", QualKind::Exempt}};
        check(RefusesFor(bare, c, "needs exactly one Player or PlayerOrId"),
              "qualifier: an Exempt qualifier on a spec with two targets is refused");
        c.args = {{"a", ArgKind::Players, false}, {"b", ArgKind::Player, false}};
        check(RefusesFor(bare, c, "needs exactly one Player or PlayerOrId"),
              "qualifier: an Exempt qualifier beside a Players and a Player argument is refused");
        c.args = {{"who", ArgKind::Player, false}};
        c.qualifiers = {{"offline", QualKind::GateOffline}};
        check(RefusesFor(bare, c, "needs a PlayerOrId argument"),
              "qualifier: a GateOffline qualifier on a Player target is refused");
        c.args = {{"who", ArgKind::PlayerOrId, false}};
        c.qualifiers = {{"exempt", QualKind::Exempt}, {"notify", QualKind::Notify}};
        check(RefusesFor(bare, c, "needs a GateOffline"),
              "qualifier: a PlayerOrId argument without a GateOffline qualifier is refused");
        c.qualifiers = {{"offline", QualKind::GateOffline}};
        check(bare.Register(c, nullptr), "qualifier: a PlayerOrId argument with GateOffline registers");
    }
    {
        Registry bare;
        CommandSpec c = ZapSpec();
        c.qualifiers = {{"offline", QualKind::GateOffline}, {"Exempt", QualKind::Exempt}};
        check(RefusesFor(bare, c, "is not 1..32 of [a-z0-9]"),
              "qualifier: a qualifier name outside [a-z0-9] is refused");
        c.qualifiers = {{"offline", QualKind::GateOffline}, {"", QualKind::Notify}};
        check(RefusesFor(bare, c, "is not 1..32 of [a-z0-9]"), "qualifier: an empty qualifier name is refused");
        c.qualifiers = {{"offline", QualKind::GateOffline}, {"notify", QualKind::Notify},
                        {"tell", QualKind::Notify}};
        check(RefusesFor(bare, c, "two qualifiers of one kind"),
              "qualifier: two qualifiers of one kind are refused");
        c.qualifiers = {{"offline", QualKind::GateOffline}, {"offline", QualKind::Exempt}};
        check(RefusesFor(bare, c, "two qualifiers named"),
              "qualifier: two qualifiers of different kinds with one name are refused");
    }
    {
        // A qualifier node is shared only by qualifiers of one kind, and no command takes one.
        Registry bare;
        check(bare.Register(ZapSpec(), nullptr), "qualifier: the zap tree registers on its own");
        CommandSpec other = ZapSpec();
        other.name = "zapother";
        other.nodeOf = "multivoid.zap";
        other.args = {{"who", ArgKind::Player, false}};
        other.qualifiers = {{"offline", QualKind::Exempt}};
        check(RefusesFor(bare, other, "qualifier of another kind"),
              "qualifier: a qualifier node reused with a different kind is refused");
        other.qualifiers.clear();
        other.nodeOf = "multivoid.zap.exempt";
        check(RefusesFor(bare, other, "names the qualifier node"),
              "qualifier: a nodeOf that names a qualifier node is refused");
    }
    {
        // A node that is not a qualifier node is never taken over by a qualifier.
        Registry bare;
        CommandSpec ban = Probe();
        ban.name = "ban";
        CommandSpec sub = Probe();
        sub.name = "notify";
        sub.defaultGranted = true;
        ban.subVerbs = {sub};
        check(bare.Register(ban, nullptr), "qualifier: a command with a notify sub-verb registers");
        CommandSpec banid = Probe();
        banid.name = "banid";
        banid.nodeOf = "multivoid.ban";
        banid.qualifiers = {{"notify", QualKind::Notify}};
        check(RefusesFor(bare, banid, "is not a qualifier node"),
              "qualifier: a qualifier node that is a command node is refused");
        check(!bare.FindNode("multivoid.ban.notify")->description.empty() &&
                  bare.FindNode("multivoid.ban.notify")->defaultGranted,
              "qualifier: the refused qualifier leaves the command node as it was");

        CommandSpec zap = ZapSpec();
        check(bare.DeclareNode({"multivoid.zap.notify", true, "A declared node."}, nullptr),
              "qualifier: a plain node declares");
        check(RefusesFor(bare, zap, "is not a qualifier node"),
              "qualifier: a qualifier node that is a DeclareNode node is refused");
    }

    {
        Registry gated;
        CommandSpec home = Probe();
        home.name = "home";
        home.description = "Brings a player to the console.";
        check(RegisterBuiltins(gated), "qualifier: builtins register");
        home.consoleOnly = true;
        check(gated.Register(home, nullptr), "qualifier: a consoleOnly spec registers");
        check(SaidOnly(Run(gated, who, "home Bob", players, everything),
                       "/home is the host's until /tp can name a destination."),
              "qualifier: a consoleOnly spec is refused to a non-operator");
        const DispatchResult h = Run(gated, who, "help", players, everything);
        check(h.ran && h.replies.size() == 2 && StartsWith(h.replies[1], "/help "),
              "qualifier: a consoleOnly spec is absent from a non-operator's help");
        check(Run(gated, console, "home Bob", players, everything).ran,
              "qualifier: the console runs a consoleOnly spec");
        const DispatchResult c = Run(gated, console, "help", players, everything);
        check(c.ran && c.replies.size() == 3 && StartsWith(c.replies[1], "/help ") &&
                  StartsWith(c.replies[2], "/home "),
              "qualifier: the console's help lists a consoleOnly spec");
    }
}

}  // namespace

void Checker::operator()(bool ok, const char* what) {
    ++total;
    if (ok) { ++pass; return; }
    UE_LOGE("commands selftest FAIL: %s", what);
}

bool RunSelftest() {
    Checker check;
    const bool breakIt = coop::config::ResolveFlag(coop::config_registry::rows::selftest_break_commands);
    SplitCases(check, breakIt);
    TargetCases(check);
    RegistryCases(check);
    QualifierCases(check);
    ModerationCases(check);
    TimeoutCases(check);
    SettingsCases(check);
    GrammarCases(check);
    MvCases(check);

    if (check.pass == check.total) {
        UE_LOGI("commands selftest: ALL PASS (%d checks)", check.total);
        return true;
    }
    UE_LOGE("commands selftest: %d/%d checks passed", check.pass, check.total);
    return false;
}

}  // namespace coop::commands

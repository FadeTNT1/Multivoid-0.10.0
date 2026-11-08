// coop/commands/commands_cases_timeout.cpp -- the cases of coop/commands/timeout_commands.h: each
// root's replies, who is told, and the arguments its verb receives, over fake ports that record into
// statics local to this file. Called from RunSelftest.

#include "coop/commands/command_dispatcher.h"
#include "coop/commands/command_registry.h"
#include "coop/commands/command_targets.h"
#include "coop/commands/commands_selftest.h"
#include "coop/commands/timeout_commands.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coop::commands {
namespace {

using coop::teleport_client::ToPlayer;
using Told = std::pair<int, std::string>;
namespace tmo = coop::timeout;

std::string Repeat(const char* s, int times) {
    std::string out;
    for (int i = 0; i < times; ++i) out += s;
    return out;
}

struct Rec {
    bool hosted = true;
    tmo::Result result = tmo::Result::Done;
    ToPlayer teleport = ToPlayer::Done;
    std::string heldId;  // the id isJailed answers true for
    int calls = 0;
    std::string verb;
    std::string id;
    std::string nick;
    int callerSlot = -9;
    uint32_t callerGen = 0;
    int targetSlot = -9;
    uint32_t targetGen = 0;
    std::vector<Told> told;
    std::vector<coop::permissions::Action> logged;
};

Rec& R() {
    static Rec rec;
    return rec;
}

tmo::Result Issue(std::string_view id, std::string_view nick) {
    R().calls++; R().verb = "issue"; R().id = std::string(id); R().nick = std::string(nick);
    return R().result;
}
tmo::Result Release(std::string_view id) {
    R().calls++; R().verb = "release"; R().id = std::string(id);
    return R().result;
}
bool IsJailed(std::string_view id) { return !R().heldId.empty() && id == R().heldId; }
ToPlayer TeleportTo(int cs, uint32_t cg, int ts, uint32_t tg) {
    R().calls++; R().verb = "tpto"; R().callerSlot = cs; R().callerGen = cg; R().targetSlot = ts; R().targetGen = tg;
    return R().teleport;
}
bool Hosted() { return R().hosted; }
void Notify(const Caller& to, std::string_view line) { R().told.emplace_back(to.slot, std::string(line)); }
void Log(const coop::permissions::Action& a) { R().logged.push_back(a); }

timeout::Ports MakePorts() {
    timeout::Ports p;
    p.hosted = &Hosted;
    p.issue = &Issue;
    p.release = &Release;
    p.isJailed = &IsJailed;
    p.teleportTo = &TeleportTo;
    p.notify = &Notify;
    p.log = &Log;
    return p;
}

PlayerView Seat(int slot, unsigned no, const char* nick, const std::string& id, uint32_t generation, bool ready) {
    PlayerView v;
    v.slot = slot;
    v.playerNo = no;
    v.nick = nick;
    v.playerId = id;
    v.generation = generation;
    v.worldReady = ready;
    return v;
}

// The host, Bob (a client), Cy (still joining) and Dee.
std::vector<PlayerView> Seated() {
    return {Seat(0, 1, "Host", Repeat("a", 32), 0, true), Seat(1, 2, "Bob", Repeat("b", 32), 7, true),
            Seat(2, 5, "Cy", Repeat("c", 32), 9, false), Seat(3, 6, "Dee", Repeat("d", 32), 4, true)};
}

bool AllowAll(const Caller&, std::string_view, bool) { return true; }
bool ExplicitNobody(std::string_view, std::string_view) { return false; }

Policy MakePolicy() {
    Policy p;
    p.check = &AllowAll;
    p.isExplicit = &ExplicitNobody;
    return p;
}

DispatchResult Run(const Registry& reg, const Caller& who, const char* line) {
    R() = Rec{};
    return Dispatch(reg, who, line, Seated(), MakePolicy());
}

DispatchResult RunWith(const Registry& reg, const Caller& who, const char* line, const Rec& setup) {
    R() = setup;
    return Dispatch(reg, who, line, Seated(), MakePolicy());
}

bool Said(const DispatchResult& r, const std::string& line) {
    return r.ran && r.replies.size() == 1 && r.replies[0] == line;
}

bool Refused(const DispatchResult& r, const std::string& line) {
    return !r.ran && r.replies.size() == 1 && r.replies[0] == line;
}

const std::string kJail = std::string(tmo::kJailName);

void TimeoutCasesOf(Checker& check, const Registry& reg, const Caller& console, const Caller& bob) {
    const std::string line = "Dee was sent to " + kJail + " for " + tmo::DurationText() + ".";
    check(Said(Run(reg, console, "timeout Dee"), line) && R().verb == "issue" && R().id == Repeat("d", 32) &&
              R().nick == "Dee",
          "timeout: /timeout holds the target by its proved id and says so");
    check(R().told == std::vector<Told>{{1, line}, {3, line}},
          "timeout: everyone ready but the host who asked is told, the target included, a joiner not");
    check(R().logged.size() == 1 && R().logged[0].description == "/timeout " + Repeat("d", 32),
          "timeout: /timeout records the action by id");
    check(Said(Run(reg, bob, "timeout Dee"), line) && R().told == std::vector<Told>{{0, line}, {3, line}},
          "timeout: a client's timeout is told to the host's feed and the target, not back to the asker");
    check(Refused(Run(reg, console, "timeout Host"), "That is the host -- it cannot be timed out.") && R().calls == 0,
          "timeout: the host cannot be held");
    {
        std::vector<PlayerView> players = Seated();
        players[3].playerId.clear();
        R() = Rec{};
        const DispatchResult r = Dispatch(reg, console, "timeout Dee", players, MakePolicy());
        check(r.replies.size() == 1 && r.replies[0] == "Dee's identity is not proved yet." && R().calls == 0,
              "timeout: a player whose id is not proved cannot be held");
    }
    Rec noId;
    noId.result = tmo::Result::NoId;
    check(Said(RunWith(reg, console, "timeout Dee", noId), "Dee's identity is not proved yet.") && R().told.empty(),
          "timeout: a refused hold tells nobody");
    Rec none;
    none.hosted = false;
    check(Said(RunWith(reg, console, "timeout Dee", none), "There is no hosted session.") && R().calls == 0,
          "timeout: outside a hosted session nothing runs");
}

void UntimeoutCasesOf(Checker& check, const Registry& reg, const Caller& console, const Caller& bob) {
    const std::string line = "Dee was let out of " + kJail + ".";
    check(Said(Run(reg, console, "untimeout Dee"), line) && R().verb == "release" && R().id == Repeat("d", 32) &&
              R().told == std::vector<Told>{{1, line}, {3, line}},
          "timeout: /untimeout releases by id and tells the others");
    Rec notHeld;
    notHeld.result = tmo::Result::NotJailed;
    check(Said(RunWith(reg, console, "untimeout Dee", notHeld), "Dee is not in " + kJail + ".") && R().told.empty(),
          "timeout: /untimeout of a player who is not held says so");
    Rec held;
    held.heldId = Repeat("b", 32);
    check(Said(RunWith(reg, bob, "untimeout Dee", held), "You are in " + kJail + ".") && R().calls == 0,
          "timeout: a held caller cannot release anyone, themselves included");
    check(Said(RunWith(reg, bob, "untimeout Bob", held), "You are in " + kJail + ".") && R().calls == 0,
          "timeout: a held caller cannot release themselves");
}

void TeleportToCasesOf(Checker& check, const Registry& reg, const Caller& console, const Caller& bob) {
    check(Said(Run(reg, console, "tpto Dee"), "Teleported you to Dee.") && R().verb == "tpto" &&
              R().callerSlot == 0 && R().callerGen == 0 && R().targetSlot == 3 && R().targetGen == 4,
          "tpto: the host goes to the target, named by slot 0 and no generation");
    check(Said(Run(reg, bob, "tpto Dee"), "Teleported you to Dee.") && R().callerSlot == 1 && R().callerGen == 7 &&
              R().targetSlot == 3 && R().targetGen == 4,
          "tpto: a client goes to the target, each seat named with its generation");
    check(R().logged.size() == 1 && R().logged[0].description == "/tpto " + Repeat("d", 32),
          "tpto: /tpto records the action by id");
    Rec held;
    held.heldId = Repeat("b", 32);
    check(Said(RunWith(reg, bob, "tpto Dee", held), "You are in " + kJail + ".") && R().calls == 0,
          "tpto: a held caller does not leave");
    struct Row { ToPlayer r; const char* reply; };
    const Row rows[] = {
        {ToPlayer::SameBody, "That is you."},
        {ToPlayer::TargetGone, "Dee already left."},
        {ToPlayer::TargetAway, "Dee is in another level."},
        {ToPlayer::TargetJoining, "Dee is still joining."},
        {ToPlayer::TargetDead, "Dee is dead."},
        {ToPlayer::NoSpot, "There is no clear spot beside Dee."},
        {ToPlayer::Failed, "Could not teleport you to Dee."},
        {ToPlayer::CallerAway, "You cannot teleport from where you are (another level, or still loading)."},
    };
    for (const Row& row : rows) {
        Rec setup;
        setup.teleport = row.r;
        check(Said(RunWith(reg, console, "tpto Dee", setup), row.reply) && R().logged.empty(),
              "tpto: a refusal is answered with its own line and records nothing");
    }
    Rec none;
    none.hosted = false;
    check(Said(RunWith(reg, console, "tpto Dee", none), "There is no hosted session.") && R().calls == 0,
          "tpto: outside a hosted session nothing runs");
}

}  // namespace

void TimeoutCases(Checker& check) {
    const Caller console{0, 0, true};
    Caller bob;
    bob.slot = 1;
    bob.generation = 7;
    bob.playerId = Repeat("b", 32);

    Registry reg;
    check(timeout::Register(reg, MakePorts()), "timeout: the three roots register");
    const CommandSpec* timeoutRoot = reg.FindRoot("timeout", nullptr);
    const CommandSpec* untimeoutRoot = reg.FindRoot("untimeout", nullptr);
    check(timeoutRoot != nullptr && untimeoutRoot != nullptr && reg.FindRoot("tpto", nullptr) != nullptr,
          "timeout: timeout, untimeout and tpto are roots");
    check(timeoutRoot != nullptr && untimeoutRoot != nullptr && reg.NodeOf(*timeoutRoot) == "multivoid.timeout" &&
              reg.NodeOf(*untimeoutRoot) == "multivoid.timeout" && reg.FindNode("multivoid.tpto") != nullptr,
          "timeout: /untimeout shares /timeout's node and /tpto has its own");
    TimeoutCasesOf(check, reg, console, bob);
    UntimeoutCasesOf(check, reg, console, bob);
    TeleportToCasesOf(check, reg, console, bob);
}

}  // namespace coop::commands

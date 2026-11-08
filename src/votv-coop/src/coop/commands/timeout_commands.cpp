// coop/commands/timeout_commands.cpp -- see coop/commands/timeout_commands.h.

#include "coop/commands/timeout_commands.h"

#include "coop/commands/action_source.h"
#include "coop/commands/command_dispatcher.h"

#include <string>
#include <utility>

namespace coop::commands::timeout {
namespace {

using coop::teleport_client::ToPlayer;

const PlayerView* TargetView(const Context& ctx) {
    const TargetResult& t = ctx.targets[0];
    if (t.slots.empty()) return nullptr;
    for (const PlayerView& v : ctx.players)
        if (v.slot == t.slots[0]) return &v;
    return nullptr;
}

void Record(const Ports& p, const Context& ctx, const PlayerView& v, const std::string& commandLine) {
    if (!p.log) return;
    coop::permissions::Action a;
    FillSource(ctx, a);
    a.targetType = "user";
    a.targetId = v.playerId;
    a.targetName = v.nick;
    a.description = commandLine + " " + (v.playerId.empty() ? std::string("unproved") : v.playerId);
    p.log(a);
}

// The line to every seated, ready player but the caller (who has it as the reply). The host is among
// the players, so its feed has it unless the host asked.
void Tell(const Ports& p, const Context& ctx, const std::string& line) {
    if (!p.notify) return;
    for (const PlayerView& v : ctx.players) {
        if (v.slot == ctx.caller.slot || !v.worldReady) continue;
        Caller to;
        to.slot = v.slot;
        to.generation = v.generation;
        to.isOperator = v.slot == 0;
        p.notify(to, line);
    }
}

// Whether the caller is a player held in the jail themselves.
bool CallerHeld(const Ports& p, const Context& ctx) {
    return ctx.caller.slot > 0 && p.isJailed && p.isJailed(ctx.caller.playerId);
}

std::string HeldRefusal() {
    return std::string("You are in ") + tmo::kJailName + ".";
}

Handler TimeoutHandler(const Ports& p) {
    return [p](Context& ctx) {
        if (!p.hosted || !p.hosted()) { ctx.Reply(kNoSession); return; }
        const PlayerView* v = TargetView(ctx);
        if (v == nullptr) { ctx.Reply("Could not find that player."); return; }
        if (v->slot < 1) { ctx.Reply("That is the host -- it cannot be timed out."); return; }
        if (v->playerId.empty()) { ctx.Reply(v->nick + "'s identity is not proved yet."); return; }
        switch (p.issue(v->playerId, v->nick)) {
            case tmo::Result::Done: {
                const std::string line =
                    v->nick + " was sent to " + tmo::kJailName + " for " + tmo::DurationText() + ".";
                ctx.Reply(line);
                Tell(p, ctx, line);
                Record(p, ctx, *v, "/timeout");
                break;
            }
            case tmo::Result::NoSession: ctx.Reply(kNoSession); break;
            case tmo::Result::NoId: ctx.Reply(v->nick + "'s identity is not proved yet."); break;
            case tmo::Result::IsHost: ctx.Reply("That is the host -- it cannot be timed out."); break;
            case tmo::Result::NotJailed: break;
        }
    };
}

Handler UntimeoutHandler(const Ports& p) {
    return [p](Context& ctx) {
        if (!p.hosted || !p.hosted()) { ctx.Reply(kNoSession); return; }
        if (CallerHeld(p, ctx)) { ctx.Reply(HeldRefusal()); return; }
        const PlayerView* v = TargetView(ctx);
        if (v == nullptr) { ctx.Reply("Could not find that player."); return; }
        switch (p.release(v->playerId)) {
            case tmo::Result::Done: {
                const std::string line = v->nick + " was let out of " + tmo::kJailName + ".";
                ctx.Reply(line);
                Tell(p, ctx, line);
                Record(p, ctx, *v, "/untimeout");
                break;
            }
            case tmo::Result::NotJailed: ctx.Reply(v->nick + " is not in " + tmo::kJailName + "."); break;
            case tmo::Result::NoSession: ctx.Reply(kNoSession); break;
            case tmo::Result::NoId:
            case tmo::Result::IsHost: ctx.Reply("Could not let " + v->nick + " out."); break;
        }
    };
}

Handler TeleportToHandler(const Ports& p) {
    return [p](Context& ctx) {
        if (!p.hosted || !p.hosted()) { ctx.Reply(kNoSession); return; }
        if (CallerHeld(p, ctx)) { ctx.Reply(HeldRefusal()); return; }
        const PlayerView* v = TargetView(ctx);
        if (v == nullptr) { ctx.Reply("Could not find that player."); return; }
        if (!p.teleportTo) { ctx.Reply("Teleporting is not available."); return; }
        // The console is the host's own player: slot 0, which has no occupancy generation.
        const int callerSlot = ctx.caller.isOperator ? 0 : ctx.caller.slot;
        const uint32_t callerGen = ctx.caller.isOperator ? 0 : ctx.caller.generation;
        switch (p.teleportTo(callerSlot, callerGen, v->slot, v->generation)) {
            case ToPlayer::Done:
                ctx.Reply("Teleported you to " + v->nick + ".");
                Record(p, ctx, *v, "/tpto");
                break;
            case ToPlayer::NoSession: ctx.Reply(kNoSession); break;
            case ToPlayer::SameBody: ctx.Reply("That is you."); break;
            case ToPlayer::CallerGone: ctx.Reply("Could not teleport you."); break;
            case ToPlayer::TargetGone: ctx.Reply(v->nick + " already left."); break;
            case ToPlayer::CallerAway:
                ctx.Reply("You cannot teleport from where you are (another level, or still loading).");
                break;
            case ToPlayer::TargetAway: ctx.Reply(v->nick + " is in another level."); break;
            case ToPlayer::TargetJoining: ctx.Reply(v->nick + " is still joining."); break;
            case ToPlayer::TargetDead: ctx.Reply(v->nick + " is dead."); break;
            case ToPlayer::NoSpot: ctx.Reply("There is no clear spot beside " + v->nick + "."); break;
            case ToPlayer::Failed: ctx.Reply("Could not teleport you to " + v->nick + "."); break;
        }
    };
}

CommandSpec TimeoutSpec(const Ports& p) {
    CommandSpec c;
    c.name = "timeout";
    c.description = std::string("Sends a player to ") + tmo::kJailName + " for " + tmo::DurationText() + ".";
    c.pastTense = "timed out";
    c.args = {{"who", ArgKind::Player, false, true}};
    c.qualifiers = {{"exempt", QualKind::Exempt}};
    c.handler = TimeoutHandler(p);
    return c;
}

// Lets a player out early; it shares /timeout's node, as /banid shares /ban's.
CommandSpec UntimeoutSpec(const Ports& p) {
    CommandSpec c;
    c.name = "untimeout";
    c.description = std::string("Lets a player out of ") + tmo::kJailName + " early.";
    c.nodeOf = "multivoid.timeout";
    c.args = {{"who", ArgKind::Player, false}};
    c.handler = UntimeoutHandler(p);
    return c;
}

CommandSpec TeleportToSpec(const Ports& p) {
    CommandSpec c;
    c.name = "tpto";
    c.description = "Takes you to a player.";
    c.args = {{"who", ArgKind::Player, false}};
    c.handler = TeleportToHandler(p);
    return c;
}

}  // namespace

bool Register(Registry& reg, const Ports& p) {
    return reg.Register(TimeoutSpec(p), nullptr) && reg.Register(UntimeoutSpec(p), nullptr) &&
           reg.Register(TeleportToSpec(p), nullptr);
}

}  // namespace coop::commands::timeout

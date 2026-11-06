// coop/commands/inventory_commands.cpp -- see coop/commands/inventory_commands.h.

#include "coop/commands/inventory_commands.h"

#include "coop/commands/action_source.h"
#include "coop/commands/command_dispatcher.h"

#include <string>

namespace coop::commands::inventory {
namespace {

const PlayerView* ViewOf(const Context& ctx, int slot) {
    for (const PlayerView& v : ctx.players)
        if (v.slot == slot) return &v;
    return nullptr;
}

Handler RestoreHandler(const Ports& p) {
    return [p](Context& ctx) {
        if (!p.hosted || !p.hosted()) { ctx.Reply("There is no hosted session."); return; }
        if (!p.restore) { ctx.Reply("Restoring an inventory is not available."); return; }
        const TargetResult& t = ctx.targets[0];
        const PlayerView* v = t.slots.empty() ? nullptr : ViewOf(ctx, t.slots[0]);
        if (v == nullptr) { ctx.Reply("Could not find that player."); return; }
        if (v->slot > 0 && !v->worldReady) { ctx.Reply(v->nick + " is still joining."); return; }
        const std::string backup = ctx.given[1] ? ctx.texts[1] : std::string();
        std::string reply;
        const bool ok = p.restore(v->slot, backup, reply);
        ctx.Reply(reply);
        if (!ok || !p.log) return;
        coop::permissions::Action a;
        FillSource(ctx, a);
        a.targetType = "user";
        a.targetId = v->playerId;
        a.targetName = v->nick;
        a.description = "/restoreinventory " + (v->playerId.empty() ? std::string("unproved") : v->playerId) + " " +
                        (backup.empty() ? std::string("good") : backup);
        p.log(a);
    };
}

CommandSpec RestoreSpec(const Ports& p) {
    CommandSpec c;
    c.name = "restoreinventory";
    c.description = "Puts a backed-up inventory back on a player.";
    c.pastTense = "restored";
    c.consoleOnly = true;
    c.args = {{"who", ArgKind::Player, false}, {"backup", ArgKind::Word, true}};
    c.handler = RestoreHandler(p);
    return c;
}

}  // namespace

bool Register(Registry& reg, const Ports& p) { return reg.Register(RestoreSpec(p), nullptr); }

}  // namespace coop::commands::inventory

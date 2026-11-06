// coop/commands/inventory_commands.h -- /restoreinventory <player> [backup]: the host puts a backed-up inventory back
// on a live player (coop/items/inventory_restore).
//
// A domain's command bindings, in the shape of coop/commands/moderation_commands: the handler calls the domain through
// the ports handed in, and the dispatcher has already decided who may run it. The command is the host console's alone
// (a client could only ask the host to do to a player what it cannot see), and the target may be the host's own
// player. `backup` is "good" (the default), "good.1", "good.2", or a relative path to a profile file in the world's
// profile folder.

#pragma once

#include "coop/commands/command_registry.h"
#include "coop/permissions/action_log.h"

#include <string>

namespace coop::commands::inventory {

struct Ports {
    // A running hosted session.
    bool (*hosted)() = nullptr;
    // The restore for the player in `slot` (0 is the host's own); the line for the caller. True when applied or sent.
    bool (*restore)(int slot, const std::string& backup, std::string& reply) = nullptr;
    // Records an admin action the verb carried out; null records nothing.
    void (*log)(const coop::permissions::Action& action) = nullptr;
};

// Registers /restoreinventory into `reg`. False when the registry refused it.
bool Register(Registry& reg, const Ports& p);

}  // namespace coop::commands::inventory

// coop/commands/timeout_commands.h -- /timeout <player>, /untimeout <player> and /tpto <player>.
//
// A domain's command bindings, in the shape of coop/commands/moderation_commands: the handlers call
// the domain through the ports handed in, captured by value, and the dispatcher has already decided who
// may run a command (the nodes multivoid.timeout and multivoid.tpto, the host's unless granted) and
// that the target is not the host where that matters. /untimeout shares /timeout's node.
//
// /timeout holds a player for coop/moderation/timeout::kSeconds in the named jail (kJailName) and tells
// everyone; /untimeout lets them out; /tpto moves the caller beside the target. A caller who is held
// themselves can do neither of the last two: they cannot talk their way out.

#pragma once

#include "coop/commands/command_registry.h"
#include "coop/commands/command_targets.h"
#include "coop/moderation/timeout.h"
#include "coop/permissions/action_log.h"
#include "coop/session/teleport_client.h"

#include <cstdint>
#include <string_view>

namespace coop::commands::timeout {

namespace tmo = coop::timeout;

struct Ports {
    // A running hosted session: the verbs act only inside one.
    bool (*hosted)() = nullptr;
    // The jail table (coop/moderation/timeout), by proved player id.
    tmo::Result (*issue)(std::string_view playerId, std::string_view nick) = nullptr;
    tmo::Result (*release)(std::string_view playerId) = nullptr;
    bool (*isJailed)(std::string_view playerId) = nullptr;
    // Move the caller (slot 0 is the host) beside the target, each seat named with its occupancy generation.
    coop::teleport_client::ToPlayer (*teleportTo)(int callerSlot, uint32_t callerGen, int targetSlot,
                                                  uint32_t targetGen) = nullptr;
    // One line to a player, now or later (a seated player, or the host's own feed).
    void (*notify)(const Caller& to, std::string_view line) = nullptr;
    // Records an admin action the verb carried out; null records nothing.
    void (*log)(const coop::permissions::Action& action) = nullptr;
};

// Registers /timeout, /untimeout and /tpto into `reg`, in that order. False when a root is refused.
bool Register(Registry& reg, const Ports& p);

}  // namespace coop::commands::timeout

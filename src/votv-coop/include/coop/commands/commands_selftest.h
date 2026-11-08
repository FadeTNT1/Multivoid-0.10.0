// coop/commands/commands_selftest.h -- the commands' un-gated boot selftest, run at every session
// start on both peers: pinned lines through the tokenizer, pinned players through the resolver, a
// test command tree through the registry and the dispatcher, the moderation, settings and `/mv`
// commands over fake ports, the grammar over test trees.

#pragma once

namespace coop::commands {

// Logs `commands selftest: ALL PASS (N checks)`, or one `commands selftest FAIL: <case>` per
// failure and `commands selftest: M/N checks passed`.
bool RunSelftest();

// The run's tally; the case files of this folder take it by reference.
struct Checker {
    int pass = 0, total = 0;
    void operator()(bool ok, const char* what);
};

// The moderation commands over fake ports (commands_cases_moderation.cpp), called by RunSelftest.
void ModerationCases(Checker& check);

// The timeout and teleport-to commands over fake ports (commands_cases_timeout.cpp), called by RunSelftest.
void TimeoutCases(Checker& check);

// The settings commands over fake ports (commands_cases_settings.cpp), called by RunSelftest.
void SettingsCases(Checker& check);

// The command grammar over test trees (commands_cases_grammar.cpp): a targeted parent, the
// argument kinds, and the nodes a tree declares (leaves only, the Notify qualifier's own node),
// called by RunSelftest.
void GrammarCases(Checker& check);

// The `/mv` commands over fake ports and an in-memory store (commands_cases_mv.cpp), called by
// RunSelftest.
void MvCases(Checker& check);

}  // namespace coop::commands

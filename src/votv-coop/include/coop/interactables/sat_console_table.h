// coop/interactables/sat_console_table.h -- which machine runs a line typed at the SAT console.
// A command whose effect or answer rests on the shared world runs on the host; one whose effect is
// the typist's own -- its compass, its terminal, its desk's presentation, its machine -- runs where it
// was typed, as does an unknown command's "err". The line is split as enterCommand splits it: the
// command is everything before the first space (the whole line when there is none, or when the
// space leads), a `debug` row is named by the rest, and every name compares without case, as the
// game's switches do. Both ends read the same table: the typist to decide what it sends, the host to
// run only what a typist may send.
#pragma once

#include <string>

namespace coop::sat_console_table {

enum class Runs { Here, OnHost };

Runs Classify(const std::wstring& line);

// A host-run command that spawns into the shared world on every call, with no limit of its own (a gift
// box, a rufus, a thiccfus): the host holds a typist to its own budget of these.
bool SpawnsEveryCall(const std::wstring& line);

// The line that empties the screen (`clr`): it runs where it was typed, and being a reset of the shared
// history it is also sent to the host, which empties the shared screen.
bool ClearsScreen(const std::wstring& line);

}  // namespace coop::sat_console_table

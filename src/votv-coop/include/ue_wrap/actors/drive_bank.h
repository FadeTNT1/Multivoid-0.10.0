// ue_wrap/actors/drive_bank.h -- the drive box's signal rows, read off and written onto a LIVE box.
//
// The engine half of the row sidecar whose arithmetic is drive_bank_rows.h. A drive box (Aprop_box_C)
// stores the signals of its drives as typed rows in `drives`, with `drives_in` saying which slots are
// occupied; 0.10.0's own getData/loadData carry the occupancy but not the rows, and the record struct has
// no `signals` member to put them in. So save_record's capture/apply of a box adds them here, in the
// record's flat `signals` sidecar (one row per slot, slot order), and a peer that receives the record has
// its slots filled rather than flagged occupied and empty. Without them the host's box sold as empty.
//
// Every access is checked: the class must be Aprop_box_C or below, `drives` must be an array of the
// measured signal row at the engine's own ElementSize (signal_dynamic::ArrayHoldsRows), `drives_in` an
// array of bytes. A class that fails refuses every read and write (said once) and touches nothing.
//
// Game thread.

#pragma once

#include "ue_wrap/actors/save_record.h"

namespace ue_wrap::drive_bank {

// If `actor` is a drive box: read its rows into `rec.signals` (one per slot; an empty slot blank).
// True when the rows were captured; false for any other actor, and for a box whose arrays do not read.
bool Capture(void* actor, ue_wrap::save_record::SaveRecord& rec);

// If `actor` is a drive box and `rec` carries rows: after the record's own loadData has set `drives_in`,
// write the rows into `drives` (occupied slots written, the others cleared) and call upd() last. A
// refused bank is left untouched and said. Returns true when the slots were written (or already right).
bool Apply(void* actor, const ue_wrap::save_record::SaveRecord& rec);

// Whether `actor` is a drive box whose arrays measured (the class gate Capture and Apply share).
bool IsBank(void* actor);

// Drop the per-class verdicts (level change / disconnect).
void Reset();

}  // namespace ue_wrap::drive_bank

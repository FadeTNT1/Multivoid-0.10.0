// ue_wrap/desk/task_grade.h -- what the drone sale's daily-task grader read, and the corner hint it raised.
//
// droneSellLocation_C::sell is the only writer of task progress (saveSlot.taskNew): it loads every drive
// box of the drone's inventory into its scratch box `ref_driveBox` and every zip floppy into `ref_zipDisc`,
// grades the rows against the task, appends each new signal id to saveSlot.soldSignals, and raises a corner
// hint per sent tape (lib_C::addHint). This is the read side of that, for coop/world/daily_task_sync's
// sale report, and the hint's own call for a peer the sale did not run on. Engine-wrapper layer
// (principle 7): no network logic. Game thread throughout.

#pragma once

#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/signal_dynamic.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::task_grade {

// saveSlot.soldSignals, the ids the grader has already taken. False while the save slot is unresolvable.
bool ReadSoldSignalIds(std::vector<std::wstring>& out);

// The sale's scratch box and zip floppy (`ref_driveBox`, `ref_zipDisc`) on a droneSellLocation_C, or null.
// They hold the LAST box and zip of the package once the body has run.
void* RefDriveBox(void* sellLocation);
void* RefZipDisc(void* sellLocation);

// Every row of a drive box's `drives`, occupied or not (the grader reads them all and keeps size > 0).
// False when the actor is not a measured drive box.
bool ReadBoxRows(void* box, std::vector<signal_dynamic::Row>& out);

// A floppy's `data` rows. Empty for null or an actor without them.
std::vector<std::wstring> ReadFloppyData(void* floppy);

// The dish a zip row names, as the grader reads it: the third "??" field with its first three characters
// cut, lower-cased. Empty when the row has no third field.
std::wstring ZipRowDish(const std::wstring& row);

// The watched lib_C::addHint call's text and type. False when the frame does not read.
bool ReadHintCall(const ue_wrap::script_gate::Call& call, std::wstring& text, uint8_t& type);

// Raise the corner hint here, as lib_C::addHint(text, type, false, gamemode). False when the library, the
// gamemode or the text mint is not available.
bool ShowHint(const std::wstring& text, uint8_t type);

}  // namespace ue_wrap::task_grade

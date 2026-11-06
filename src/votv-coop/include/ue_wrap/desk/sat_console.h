// ue_wrap/desk/sat_console.h -- the SAT console: the one ui_console_C terminal a machine's
// panel_SATconsole_C panels all show (the desk hands its atlas's umg_console to every panel as it
// begins), and terminals of our own made of the same class and never shown. A terminal runs a typed
// line through enterCommand, prints every line through writeToLog, and is busy while `processing`
// is set; a panel points it at its dish with init(name, hide, dish) and records itself in `used`.
// Engine-wrapper layer (principle 7): no network or gameplay logic.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ue_wrap::sat_console {

// Resolve the terminal's members and verbs, the atlas's terminal slot, the desk's panel list and the
// widget library's Create; a throttled lazy retry that latches off after a few passes with the classes
// loaded. Game thread.
bool EnsureResolved();

// This machine's terminal: the desk atlas's umg_console. Null while the desk is not live.
void* LocalTerminal();

// A new terminal of the game's class, made by WidgetBlueprintLibrary.Create with no owning player:
// its outer is the game instance, and the engine binds the widgets its body writes to. Nothing adds it
// to a viewport, so nothing builds it into Slate and its Construct -- which would bind the world's
// broken-device alerts to it -- never runs. The caller pins it (GcPin), gives it a panel, and discards
// it when the session ends or once its world is no longer the current one. Null on failure. Game thread.
void* CreateTerminal(void* worldContext);

// A terminal we made, done with: marked for destruction now, so the Delays of a command it was still
// running stop with it rather than whenever the collector gets to it. Its pin must be released first.
void DiscardTerminal(void* term);

// The arguments of a watched call, read from its parameter frame: enterCommand's line, and
// writeToLog's text and bracket type (0 an output line, 1 the typed line's echo, 2 bare).
bool ReadEnterCommandLine(const uint8_t* locals, std::wstring& out);
bool ReadWriteToLogArgs(const uint8_t* locals, std::wstring& text, uint8_t& bracketsType);

// The terminal's state.
bool  ReadProcessing(void* term, bool& out);
bool  WriteProcessing(void* term, bool busy);
bool  WritePanel(void* term, void* desk);
void* ReadUsed(void* term);                  // the panel the player last used, or null
bool  WriteUsed(void* term, void* panel);
void* ReadActiveDish(void* term);            // the dish init pointed it at, or null (ROOT)
bool  ReadName(void* term, std::wstring& out);
bool  ReadLog(void* term, std::wstring& out);  // the printed lines, the last 3000 characters (fixLog)

// The screen, as two peers show it alike. The live input is the text box's text, or `command` when the
// alternate keyboard path typed into it instead; a shown input is written to both and redrawn. A shown log
// is written to consoleLine and repainted through the terminal's own fixLog, which also keeps its
// rightmost 3000 characters. None of these runs a command, and none prints through writeToLog.
// ScreenReady: the members these need are all resolved (the miss is logged once).
bool ScreenReady();
bool ReadInput(void* term, std::wstring& out);
bool WriteInput(void* term, const std::wstring& text);
bool WriteLog(void* term, const std::wstring& text);

// Whether one of this terminal's own bodies (enterCommand, or its ubergraph, which carries the latent
// continuations) is running on this thread: a line printed inside one belongs to a command, and one printed
// outside any is the world's own alert.
bool InTerminalBody(void* term);

// The terminal's verbs, as the game calls them. RunCommand runs a line as the input's commit does:
// the input `command` holds the line, then enterCommand(line), which echoes `command`.
// ConsumeInput empties the input as enterCommand's own tail does: `command` and the text box.
bool CallInit(void* term, const std::wstring& name, bool hide, void* dish);
bool RunCommand(void* term, const std::wstring& line);
bool CallWriteToLog(void* term, const std::wstring& text, uint8_t bracketsType);
bool ConsumeInput(void* term);

// The panels, named by the placed actor's name, which the map gives every machine alike: a panel's
// name, and this machine's panel of that name in the desk's list, or null.
std::wstring PanelName(void* panel);
void* FindPanelByName(const std::wstring& name);

}  // namespace ue_wrap::sat_console

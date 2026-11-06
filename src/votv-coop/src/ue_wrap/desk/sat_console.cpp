// ue_wrap/desk/sat_console.cpp -- see ue_wrap/desk/sat_console.h.

#include "ue_wrap/desk/sat_console.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"  // IsBodyActive: is one of the terminal's own bodies running
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/desk/console_desk.h"  // Instance, AtlasWidget

#include <chrono>

namespace ue_wrap::sat_console {
namespace {

namespace R = reflection;
namespace N = profile::name;
using field_io::FStringView;
using field_io::ReadFStringAt;
using field_io::TArrayView;
using field_io::WriteFStringField;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// A member missing from a loaded class does not appear later: the retry covers the window where the
// classes are still loading, and then latches off with one line.
constexpr int kMaxPostClassAttempts = 5;

// The terminal class, found again in a world that loaded it anew; its Blueprint functions are asked
// through the memoised dispatch lookup at each call for the same reason. The offsets are the class
// layout's and outlive a reload.
CachedObjRef g_termCls;
int32_t g_offProcessing = -1;
uint8_t g_maskProcessing = 0;
int32_t g_offPanel = -1, g_offUsed = -1, g_offActiveDish = -1;
int32_t g_offName = -1, g_offCommand = -1, g_offTextBox = -1, g_offLog = -1;
int32_t g_offEnterC = -1;  // enterCommand's c
int32_t g_offWriteB = -1, g_offWriteType = -1;  // writeToLog's B and bracketsType
bool    g_haveFixLog = false;  // the terminal declares the verb that trims its history and repaints it
bool    g_haveUpd = false;     // ... and the one that redraws its input
bool    g_saidNoGetText = false;

int32_t g_offAtlasTerm = -1;  // ui_consolesAtlas_C.umg_console
int32_t g_offDeskPanels = -1;  // analogDScreenTest_C.consoles

void*   g_fnSetText = nullptr;  // EditableTextBox.SetText
void*   g_fnGetText = nullptr;  // EditableTextBox.GetText; the live input is read through it
void*   g_wblCdo = nullptr;
void*   g_fnCreate = nullptr;  // WidgetBlueprintLibrary.Create

bool     g_resolved = false;
bool     g_latchedOff = false;
int      g_attempts = 0;
uint64_t g_nextTryMs = 0;

bool Complete() {
    return g_offProcessing >= 0 && g_maskProcessing && g_offPanel >= 0 && g_offUsed >= 0 &&
           g_offActiveDish >= 0 && g_offName >= 0 && g_offCommand >= 0 && g_offTextBox >= 0 && g_offLog >= 0 &&
           g_offEnterC >= 0 && g_offWriteB >= 0 &&
           g_offWriteType >= 0 && g_offAtlasTerm >= 0 && g_offDeskPanels >= 0 && g_fnSetText && g_wblCdo &&
           g_fnCreate && g_haveFixLog;  // the history is repainted by fixLog: a build without it is not this one
}

template <class T>
T* At(void* base, int32_t off) {
    return reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off);
}

void* ReadObject(void* base, int32_t off) {
    if (!base || off < 0) return nullptr;
    void* o = *At<void*>(base, off);
    return (o && R::IsLive(o)) ? o : nullptr;
}

bool WriteObject(void* base, int32_t off, void* value) {
    if (!base || off < 0) return false;
    *At<void*>(base, off) = value;
    return true;
}

void* TerminalClass() {
    void* cls = g_termCls.Get();
    if (!cls) {
        cls = R::FindClass(N::SatConsoleClass);
        g_termCls.Set(cls);
    }
    return cls;
}

bool IsTerminal(void* obj) {
    return obj && R::IsLive(obj) && R::ClassOf(obj) == TerminalClass();
}

void* TerminalFn(void* term, const wchar_t* name) { return R::FindDispatchFunctionCached(R::ClassOf(term), name); }

// A property found by name AND by its type: the screen's writes go to a string slot, and a slot of another
// type is no slot of ours. The resolution summary names the member that missed.
int32_t StrPropertyOffset(void* cls, const wchar_t* name) {
    int32_t off = -1;
    std::wstring type;
    if (!R::PropertyOffsetAndClass(cls, name, off, type) || type != L"StrProperty") return -1;
    return off;
}

// A verb of the terminal that takes nothing.
bool CallNoParams(void* term, const wchar_t* fnName) {
    ParamFrame f(TerminalFn(term, fnName));
    return f.valid() && Call(term, f);
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved) return true;
    if (g_latchedOff) return false;
    const uint64_t now = NowMs();
    if (now < g_nextTryMs) return false;
    g_nextTryMs = now + 1000;

    void* term = R::FindClass(N::SatConsoleClass);
    void* atlas = R::FindClass(N::SatConsoleAtlasClass);
    void* desk = R::FindClass(N::SatConsoleDeskClass);
    void* box = R::FindClass(N::EditableTextBoxClass);
    if (!term || !atlas || !desk || !box) return false;  // the world is still loading

    g_termCls.Set(term);
    R::FindBoolProperty(term, N::SatConsoleProcessingProp, g_offProcessing, g_maskProcessing);
    g_offPanel      = R::FindPropertyOffset(term, N::SatConsolePanelProp);
    g_offUsed       = R::FindPropertyOffset(term, N::SatConsoleUsedProp);
    g_offActiveDish = R::FindPropertyOffset(term, N::SatConsoleActiveDishProp);
    g_offName       = R::FindPropertyOffset(term, N::SatConsoleNameProp);
    g_offCommand    = StrPropertyOffset(term, N::SatConsoleCommandProp);
    g_offTextBox    = R::FindPropertyOffset(term, N::SatConsoleTextBoxProp);
    g_offLog        = StrPropertyOffset(term, N::SatConsoleLogProp);
    g_haveFixLog    = R::FindFunction(term, N::SatConsoleFixLogFn) != nullptr;
    g_haveUpd       = R::FindFunction(term, N::SatConsoleUpdFn) != nullptr;
    void* fnEnter = R::FindFunction(term, N::SatConsoleEnterCommandFn);
    void* fnWrite = R::FindFunction(term, N::SatConsoleWriteToLogFn);
    void* fnInit  = R::FindFunction(term, N::SatConsoleInitFn);
    if (fnEnter) g_offEnterC = R::FindParamOffset(fnEnter, L"c");
    if (fnWrite) {
        g_offWriteB    = R::FindParamOffset(fnWrite, L"B");
        g_offWriteType = R::FindParamOffset(fnWrite, L"bracketsType");
    }
    const bool haveInit = fnInit && R::FindParamOffset(fnInit, L"name") >= 0 && R::FindParamOffset(fnInit, L"dish") >= 0;
    g_offAtlasTerm  = R::FindPropertyOffset(atlas, N::SatConsoleAtlasTerminalProp);
    g_offDeskPanels = R::FindPropertyOffset(desk, N::SatConsoleDeskPanelsProp);
    g_fnSetText = R::FindFunction(box, N::EditableTextBoxSetTextFn);
    g_fnGetText = R::FindFunction(box, N::EditableTextBoxGetTextFn);
    g_wblCdo = R::FindClassDefaultObject(L"WidgetBlueprintLibrary");
    if (g_wblCdo) g_fnCreate = R::FindFunction(R::ClassOf(g_wblCdo), L"Create");

    // No offset fallbacks: a member this wrapper cannot find means the class is not the one it was
    // written against, and a guessed offset would write into whatever lives there now.
    if (!Complete() || !haveInit) {
        if (++g_attempts >= kMaxPostClassAttempts) {
            g_latchedOff = true;
            UE_LOGW("sat_console: resolution incomplete after %d passes (processing=%d panel=%d used=%d "
                    "activeDish=%d name=%d command=%d textBox=%d log=%d init=%d c=%d B=%d "
                    "type=%d atlasTerm=%d panels=%d setText=%p create=%p fixLog=%d) -- the terminal stays off; game "
                    "version mismatch? (command and consoleLine must be StrProperty)", g_attempts, g_offProcessing,
                    g_offPanel, g_offUsed, g_offActiveDish, g_offName, g_offCommand, g_offTextBox, g_offLog,
                    haveInit ? 1 : 0, g_offEnterC, g_offWriteB, g_offWriteType, g_offAtlasTerm, g_offDeskPanels,
                    g_fnSetText, g_fnCreate, g_haveFixLog ? 1 : 0);
        }
        return false;
    }
    g_resolved = true;
    if (!g_fnGetText || !g_haveUpd)
        UE_LOGW("sat_console: the shared input is read and drawn less exactly (GetText=%p upd=%d) -- `command` alone "
                "is read", g_fnGetText, g_haveUpd ? 1 : 0);
    UE_LOGI("sat_console: resolved (processing=0x%X/%02X command=0x%X log=0x%X c=0x%X B=0x%X)", g_offProcessing,
            g_maskProcessing, g_offCommand, g_offLog, g_offEnterC, g_offWriteB);
    return true;
}

void* LocalTerminal() {
    if (!EnsureResolved()) return nullptr;
    void* atlas = console_desk::AtlasWidget();
    void* term = ReadObject(atlas, g_offAtlasTerm);
    return IsTerminal(term) ? term : nullptr;
}

void* CreateTerminal(void* worldContext) {
    if (!worldContext || !EnsureResolved()) return nullptr;
    ParamFrame f(g_fnCreate);
    void* cls = TerminalClass();
    if (!cls || !f.valid() || !f.Set(L"WorldContextObject", worldContext) || !f.Set(L"WidgetType", cls))
        return nullptr;
    void* noPlayer = nullptr;  // the game instance owns it: no world object is its outer
    if (!f.Set(L"OwningPlayer", noPlayer) || !Call(g_wblCdo, f)) return nullptr;
    void* made = f.Get<void*>(L"ReturnValue");
    return IsTerminal(made) ? made : nullptr;
}

void DiscardTerminal(void* term) {
    if (IsTerminal(term)) R::MarkPendingKill(term);
}

bool ReadEnterCommandLine(const uint8_t* locals, std::wstring& out) {
    if (!locals || !g_resolved) return false;
    out = ReadFStringAt(locals, g_offEnterC);
    return true;
}

bool ReadWriteToLogArgs(const uint8_t* locals, std::wstring& text, uint8_t& bracketsType) {
    if (!locals || !g_resolved) return false;
    text = ReadFStringAt(locals, g_offWriteB);
    bracketsType = locals[g_offWriteType];
    return true;
}

bool ReadProcessing(void* term, bool& out) {
    if (!g_resolved || !IsTerminal(term)) return false;
    out = (*At<uint8_t>(term, g_offProcessing) & g_maskProcessing) != 0;
    return true;
}

bool WriteProcessing(void* term, bool busy) {
    if (!g_resolved || !IsTerminal(term)) return false;
    uint8_t& b = *At<uint8_t>(term, g_offProcessing);
    b = busy ? static_cast<uint8_t>(b | g_maskProcessing) : static_cast<uint8_t>(b & ~g_maskProcessing);
    return true;
}

bool WritePanel(void* term, void* desk) { return g_resolved && IsTerminal(term) && WriteObject(term, g_offPanel, desk); }
void* ReadUsed(void* term) { return (g_resolved && IsTerminal(term)) ? ReadObject(term, g_offUsed) : nullptr; }
bool WriteUsed(void* term, void* panel) { return g_resolved && IsTerminal(term) && WriteObject(term, g_offUsed, panel); }

void* ReadActiveDish(void* term) {
    return (g_resolved && IsTerminal(term)) ? ReadObject(term, g_offActiveDish) : nullptr;
}

bool ReadName(void* term, std::wstring& out) {
    if (!g_resolved || !IsTerminal(term)) return false;
    out = ReadFStringAt(term, g_offName);
    return true;
}

bool ReadLog(void* term, std::wstring& out) {
    if (!g_resolved || !IsTerminal(term)) return false;
    out = ReadFStringAt(term, g_offLog);
    return true;
}

bool CallInit(void* term, const std::wstring& name, bool hide, void* dish) {
    if (!g_resolved || !IsTerminal(term)) return false;
    ParamFrame f(TerminalFn(term, N::SatConsoleInitFn));
    if (!f.valid()) return false;
    // The frame's string may point at our buffer for the call: init copies it (ToLower) before it
    // keeps anything.
    FStringView v{const_cast<wchar_t*>(name.c_str()), static_cast<int32_t>(name.size() + 1),
                  static_cast<int32_t>(name.size() + 1)};
    const uint8_t h = hide ? 1 : 0;
    if (!f.SetRaw(L"name", &v, sizeof(v)) || !f.SetRaw(L"hide", &h, sizeof(h)) || !f.Set(L"dish", dish))
        return false;
    return Call(term, f);
}

bool RunCommand(void* term, const std::wstring& line) {
    if (!g_resolved || !IsTerminal(term)) return false;
    if (!WriteFStringField(term, g_offCommand, line)) return false;
    ParamFrame f(TerminalFn(term, N::SatConsoleEnterCommandFn));
    if (!f.valid()) return false;
    // enterCommand copies c into its locals and the ubergraph frame before it keeps anything.
    FStringView v{const_cast<wchar_t*>(line.c_str()), static_cast<int32_t>(line.size() + 1),
                  static_cast<int32_t>(line.size() + 1)};
    if (!f.SetRaw(L"c", &v, sizeof(v))) return false;
    return Call(term, f);
}

bool CallWriteToLog(void* term, const std::wstring& text, uint8_t bracketsType) {
    if (!g_resolved || !IsTerminal(term)) return false;
    ParamFrame f(TerminalFn(term, N::SatConsoleWriteToLogFn));
    if (!f.valid()) return false;
    // writeToLog concatenates B into a new string before it keeps anything.
    FStringView v{const_cast<wchar_t*>(text.c_str()), static_cast<int32_t>(text.size() + 1),
                  static_cast<int32_t>(text.size() + 1)};
    if (!f.SetRaw(L"B", &v, sizeof(v)) || !f.SetRaw(L"bracketsType", &bracketsType, sizeof(bracketsType)))
        return false;
    return Call(term, f);
}

bool ConsumeInput(void* term) {
    if (!g_resolved || !IsTerminal(term)) return false;
    if (!WriteFStringField(term, g_offCommand, std::wstring())) return false;
    void* box = ReadObject(term, g_offTextBox);
    if (!box) return true;  // no text box: the input was the command alone
    uint8_t empty[ftext_utils::kFTextSize];
    if (!ftext_utils::EmptyFText(empty)) return false;
    ParamFrame f(g_fnSetText);
    if (!f.valid() || !f.SetRaw(L"InText", empty, sizeof(empty))) return false;
    return Call(box, f);
}

bool ScreenReady() { return g_resolved; }

bool ReadInput(void* term, std::wstring& out) {
    if (!g_resolved || !IsTerminal(term)) return false;
    out.clear();
    void* box = ReadObject(term, g_offTextBox);
    if (box && g_fnGetText) {
        ParamFrame f(g_fnGetText);
        uint8_t text[ftext_utils::kFTextSize] = {};
        // The returned FText's reference is not released (a ParamFrame frees without destructing); a read is
        // made per typed character at most, and the data it names is the box's own.
        if (f.valid() && Call(box, f) && f.GetRaw(L"ReturnValue", text, sizeof(text)))
            out = ftext_utils::FTextToString(text);
    } else if (!g_saidNoGetText) {
        g_saidNoGetText = true;
        UE_LOGW("sat_console: no GetText on the input box -- the input is read from `command` alone");
    }
    if (out.empty()) out = ReadFStringAt(term, g_offCommand);  // the alternate keyboard path types into it
    return true;
}

bool WriteInput(void* term, const std::wstring& text) {
    if (!g_resolved || !IsTerminal(term)) return false;
    if (!WriteFStringField(term, g_offCommand, text)) return false;
    void* box = ReadObject(term, g_offTextBox);
    if (box && g_fnSetText) {
        uint8_t ft[ftext_utils::kFTextSize];
        if (!(text.empty() ? ftext_utils::EmptyFText(ft) : ftext_utils::MintFText(text.c_str(), ft))) return false;
        ParamFrame f(g_fnSetText);
        if (!f.valid() || !f.SetRaw(L"InText", ft, sizeof(ft)) || !Call(box, f)) return false;
    }
    if (g_haveUpd) CallNoParams(term, N::SatConsoleUpdFn);  // text_command = command + "|"
    return true;
}

bool WriteLog(void* term, const std::wstring& text) {
    if (!g_resolved || !IsTerminal(term)) return false;
    if (!WriteFStringField(term, g_offLog, text)) return false;
    return CallNoParams(term, N::SatConsoleFixLogFn);  // keeps the rightmost 3000 characters, repaints
}

bool InTerminalBody(void* term) {
    if (!g_resolved || !IsTerminal(term)) return false;
    return script_gate::IsBodyActive(TerminalFn(term, N::SatConsoleEnterCommandFn)) ||
           script_gate::IsBodyActive(TerminalFn(term, N::SatConsoleUbergraphFn));
}

std::wstring PanelName(void* panel) {
    if (!panel || !R::IsLive(panel)) return {};
    return R::ToString(R::NameOf(panel));
}

void* FindPanelByName(const std::wstring& name) {
    if (name.empty() || !EnsureResolved()) return nullptr;
    void* desk = console_desk::Instance();
    if (!desk) return nullptr;
    const auto* arr = At<TArrayView>(desk, g_offDeskPanels);
    if (!arr->data || arr->num <= 0) return nullptr;
    void* const* elems = reinterpret_cast<void* const*>(arr->data);
    for (int32_t i = 0; i < arr->num; ++i) {
        void* p = elems[i];
        if (p && R::IsLive(p) && R::ToString(R::NameOf(p)) == name) return p;
    }
    return nullptr;
}

}  // namespace ue_wrap::sat_console

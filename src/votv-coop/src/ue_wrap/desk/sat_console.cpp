// ue_wrap/desk/sat_console.cpp -- see ue_wrap/desk/sat_console.h.

#include "ue_wrap/desk/sat_console.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"  // IsBodyActive: is one of the terminal's own bodies running
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/desk/console_desk.h"  // Instance, AtlasWidget

#include <chrono>
#include <cwchar>
#include <vector>

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
uint64_t g_nextRepairMs = 0;  // the widget chain's repair is tried at most once a second

// The tick-offscreen hold: the desk's widget component, and the value it had before the hold set it.
struct OffscreenHold {
    void*   comp = nullptr;
    int32_t idx = -1;
    bool    changed = false;
    bool    prior = false;
};
OffscreenHold g_hold;
uint64_t g_nextHoldMs = 0;
bool     g_saidNoComp = false, g_saidNoTickFn = false, g_saidHeld = false;

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

namespace {

// The widget a widget component shows, by the component's reflected reader: GetUserWidgetObject, or GetWidget in a
// build that names it so. Null when neither resolves or the component shows none.
void* ComponentWidget(void* comp) {
    if (!comp || !R::IsLive(comp)) return nullptr;
    for (const wchar_t* name : {N::WidgetComponentGetWidgetFn, N::WidgetComponentGetWidgetAltFn}) {
        void* fn = R::FindDispatchFunctionCached(R::ClassOf(comp), name);
        if (!fn) continue;
        ParamFrame f(fn);
        if (!f.valid() || !Call(comp, f)) continue;
        void* w = f.Get<void*>(L"ReturnValue");
        if (w && R::IsLive(w)) return w;
    }
    return nullptr;
}

struct CompScan {
    void*   desk;
    void*   found[8];
    int     n;
};

// The desk's widget component that holds the console's atlas: the desk's own components are the WidgetComponents
// whose outer it is (found by class and by outer, not by a name this wrapper would have to guess).
void* FindAtlasComponent(void* desk, void** widgetOut) {
    if (widgetOut) *widgetOut = nullptr;
    void* compCls = R::FindClass(N::WidgetComponentClass);
    void* atlasCls = R::FindClass(N::SatConsoleAtlasClass);
    if (!desk || !compCls || !atlasCls) return nullptr;
    CompScan scan{desk, {}, 0};
    object_index::ForEachInstance(compCls, [](void* ctx, void* obj, int32_t index) {
        auto* sc = static_cast<CompScan*>(ctx);
        if (!obj || sc->n >= 8) return;
        if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
        if (R::OuterOf(obj) == sc->desk) sc->found[sc->n++] = obj;
    }, &scan);
    for (int i = 0; i < scan.n; ++i) {
        void* w = ComponentWidget(scan.found[i]);
        if (w && R::ClassOf(w) == atlasCls) {
            if (widgetOut) *widgetOut = w;
            return scan.found[i];
        }
    }
    return nullptr;
}

struct TermScan {
    void* atlas;
    void* found;
};

// The terminal the atlas's widget tree holds: an instance of the terminal's class named like the atlas's slot
// (umg_console) whose outer chain reaches the atlas. The terminals the host makes for its typists have the game
// instance for an outer and are never this one.
void* FindAtlasTerminal(void* atlas) {
    void* cls = TerminalClass();
    if (!atlas || !cls) return nullptr;
    TermScan scan{atlas, nullptr};
    object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t index) {
        auto* sc = static_cast<TermScan*>(ctx);
        if (sc->found || !obj) return;
        if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
        if (R::ToString(R::NameOf(obj)) != N::SatConsoleAtlasTerminalProp) return;
        void* o = R::OuterOf(obj);
        for (int hop = 0; o && hop < 4; ++hop, o = R::OuterOf(o))
            if (o == sc->atlas) {
                sc->found = obj;
                return;
            }
    }, &scan);
    return IsTerminal(scan.found) ? scan.found : nullptr;
}

// A missing link rebound from what already holds it. Never BeginPlay, never a use: a pointer written where the
// desk's own begin would have put the same one.
void* RepairLocal() {
    const uint64_t now = NowMs();
    if (now < g_nextRepairMs) return nullptr;
    g_nextRepairMs = now + 1000;
    void* desk = console_desk::Instance();
    if (!desk) return nullptr;
    void* atlas = console_desk::AtlasWidget();
    if (!atlas) {
        void* w = nullptr;
        void* comp = FindAtlasComponent(desk, &w);
        if (!comp || !w || !console_desk::BindAtlasWidget(w)) return nullptr;
        UE_LOGW("sat_console: the desk's Widget backref was empty while its widget component holds the atlas -- "
                "bound again (desk %p component %p atlas %p)", desk, comp, w);
        atlas = w;
    }
    void* term = ReadObject(atlas, g_offAtlasTerm);
    if (IsTerminal(term)) return term;
    term = FindAtlasTerminal(atlas);
    if (!term || !WriteObject(atlas, g_offAtlasTerm, term)) return nullptr;
    UE_LOGW("sat_console: the atlas's umg_console was empty while its widget tree holds the terminal -- bound again "
            "(atlas %p terminal %p)", atlas, term);
    return term;
}

}  // namespace

void* LocalTerminal() {
    if (!EnsureResolved()) return nullptr;
    void* atlas = console_desk::AtlasWidget();
    void* term = ReadObject(atlas, g_offAtlasTerm);
    if (IsTerminal(term)) return term;
    return RepairLocal();
}

const char* StatusText(ScreenStatus s) {
    switch (s) {
    case ScreenStatus::Ready:      return "ready";
    case ScreenStatus::Unresolved: return "schema unresolved";
    case ScreenStatus::NoDesk:     return "no desk";
    case ScreenStatus::NoAtlas:    return "no desk widget";
    case ScreenStatus::NoTerminal: return "no terminal in the widget";
    }
    return "?";
}

ScreenDiag DiagnoseLocal() {
    ScreenDiag d;
    if (!g_resolved) return d;
    d.desk = console_desk::Instance();
    if (!d.desk) {
        d.status = ScreenStatus::NoDesk;
        return d;
    }
    d.atlas = console_desk::AtlasWidget();
    void* term = d.atlas ? ReadObject(d.atlas, g_offAtlasTerm) : nullptr;
    if (IsTerminal(term)) {
        d.terminal = term;
        d.status = ScreenStatus::Ready;
        return d;
    }
    d.status = d.atlas ? ScreenStatus::NoTerminal : ScreenStatus::NoAtlas;
    void* w = nullptr;
    d.widgetComponent = FindAtlasComponent(d.desk, &w);
    d.componentHoldsAtlas = d.widgetComponent && w;
    return d;
}

namespace {

// The name of a function's one input parameter when it is a single byte (a bool) and the function takes nothing
// else; empty when the signature is anything other than that. A setter whose signature is not what this wrapper was
// written against is not called.
std::wstring OneBoolParam(void* fn) {
    if (!fn) return {};
    std::wstring name;
    int inputs = 0;
    for (const R::ParamInfo& p : R::FunctionParams(fn)) {
        if (p.flags & profile::cpf::ReturnParm) continue;
        ++inputs;
        if (p.size == 1) name = p.name;
    }
    return inputs == 1 ? name : std::wstring();
}

bool ReadTickOffscreen(void* comp, bool& out) {
    void* cls = R::ClassOf(comp);
    if (void* getFn = R::FindDispatchFunctionCached(cls, N::WidgetComponentGetTickWhenOffscreenFn)) {
        ParamFrame f(getFn);
        uint8_t v = 0;
        if (f.valid() && Call(comp, f) && f.GetRaw(L"ReturnValue", &v, 1)) {
            out = v != 0;
            return true;
        }
    }
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(cls, N::WidgetComponentTickWhenOffscreenProp, off, mask) || off < 0 || !mask) return false;
    out = (*At<uint8_t>(comp, off) & mask) != 0;
    return true;
}

bool SetTickOffscreen(void* comp, bool on) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(comp), N::WidgetComponentSetTickWhenOffscreenFn);
    const std::wstring param = OneBoolParam(fn);
    if (param.empty()) return false;
    ParamFrame f(fn);
    const uint8_t v = on ? 1 : 0;
    return f.valid() && f.SetRaw(param.c_str(), &v, 1) && Call(comp, f);
}

void ReleaseHold() {
    if (g_hold.comp && g_hold.changed && R::IsLiveByIndex(g_hold.comp, g_hold.idx))
        SetTickOffscreen(g_hold.comp, g_hold.prior);
    g_hold = OffscreenHold{};
}

}  // namespace

bool HoldOffscreenTick(bool hold) {
    if (!hold) {
        ReleaseHold();
        return true;
    }
    if (g_hold.comp && R::IsLiveByIndex(g_hold.comp, g_hold.idx)) return true;
    g_hold = OffscreenHold{};  // a component of a world gone: nothing of ours to put back
    const uint64_t now = NowMs();
    if (now < g_nextHoldMs || !EnsureResolved()) return false;
    g_nextHoldMs = now + 2000;
    void* desk = console_desk::Instance();
    if (!desk) return false;
    void* comp = FindAtlasComponent(desk, nullptr);
    if (!comp) {
        if (!g_saidNoComp) {
            g_saidNoComp = true;
            UE_LOGW("sat_console: the desk has no widget component that holds the console's atlas -- the terminal is "
                    "not kept ticking offscreen");
        }
        return false;
    }
    bool prior = false;
    if (!ReadTickOffscreen(comp, prior) || OneBoolParam(R::FindDispatchFunctionCached(
            R::ClassOf(comp), N::WidgetComponentSetTickWhenOffscreenFn)).empty()) {
        if (!g_saidNoTickFn) {
            g_saidNoTickFn = true;
            UE_LOGW("sat_console: the widget component's tick-offscreen switch does not resolve here (setter=%p "
                    "getter=%p prop=%d SetTickMode=%p) -- the terminal is not kept ticking offscreen; its "
                    "SetTickMode takes an enum whose values are not reflected, so it is not driven",
                    R::FindDispatchFunctionCached(R::ClassOf(comp), N::WidgetComponentSetTickWhenOffscreenFn),
                    R::FindDispatchFunctionCached(R::ClassOf(comp), N::WidgetComponentGetTickWhenOffscreenFn),
                    R::FindPropertyOffset(R::ClassOf(comp), N::WidgetComponentTickWhenOffscreenProp) >= 0 ? 1 : 0,
                    R::FindDispatchFunctionCached(R::ClassOf(comp), N::WidgetComponentSetTickModeFn));
        }
        return false;
    }
    g_hold.comp = comp;
    g_hold.idx = R::InternalIndexOf(comp);
    if (prior) return true;  // it ticks offscreen already: nothing to set, nothing to put back
    if (!SetTickOffscreen(comp, true)) {
        g_hold = OffscreenHold{};
        return false;
    }
    g_hold.changed = true;
    g_hold.prior = false;
    if (!g_saidHeld) {
        g_saidHeld = true;
        UE_LOGI("sat_console: the desk's widget component %p ticks offscreen while a remote typist holds the console", comp);
    }
    return true;
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

// ---- the terminals ----

namespace {

CachedObjRef g_panelCls;
int32_t  g_offPanelRoot = -1;
uint8_t  g_maskPanelRoot = 0;
int32_t  g_offPanelDish = -1;
int32_t  g_offDishName = -1;
int32_t  g_offOverlapActor = -1;
bool     g_panelResolved = false;
bool     g_panelSaidMiss = false;
uint64_t g_panelNextTryMs = 0;

void* PanelClass() {
    void* cls = g_panelCls.Get();
    if (!cls) {
        cls = R::FindClass(N::SatPanelClass);
        g_panelCls.Set(cls);
    }
    return cls;
}

// The panel's members, by name and type: the root flag (a bool, masked), the dish (an object), the dish's name
// (a text), and the begin-overlap event's OtherActor. A miss is said once; the walk is retried at most once a second
// while the classes load.
bool EnsurePanelResolved() {
    if (g_panelResolved) return true;
    const uint64_t now = NowMs();
    if (now < g_panelNextTryMs) return false;
    g_panelNextTryMs = now + 1000;
    void* cls = PanelClass();
    void* dishCls = R::FindClass(N::DishClass);
    if (!cls || !dishCls) return false;
    R::FindBoolProperty(cls, N::SatPanelRootProp, g_offPanelRoot, g_maskPanelRoot);
    std::wstring type;
    int32_t off = -1;
    g_offPanelDish = (R::PropertyOffsetAndClass(cls, N::SatPanelDishProp, off, type) && type == L"ObjectProperty") ? off : -1;
    off = -1;
    type.clear();
    g_offDishName = (R::PropertyOffsetAndClass(dishCls, N::DishNameProp, off, type) && type == L"TextProperty") ? off : -1;
    if (void* fn = R::FindFunction(cls, N::SatPanelOverlapFn)) g_offOverlapActor = R::FindParamOffset(fn, N::SatPanelOverlapActorParam);
    if (g_offPanelRoot < 0 || !g_maskPanelRoot || g_offPanelDish < 0 || g_offDishName < 0) {
        if (!g_panelSaidMiss) {
            g_panelSaidMiss = true;
            UE_LOGW("sat_console: the panel's members do not resolve (root=%d/%02X control_asDish=%d nameDish=%d) -- no "
                    "terminal has a context; game version mismatch?", g_offPanelRoot, g_maskPanelRoot, g_offPanelDish,
                    g_offDishName);
        }
        return false;
    }
    g_panelResolved = true;
    UE_LOGI("sat_console: panels resolved (root=0x%X/%02X control_asDish=0x%X nameDish=0x%X overlap OtherActor=%d)",
            g_offPanelRoot, g_maskPanelRoot, g_offPanelDish, g_offDishName, g_offOverlapActor);
    return true;
}

uint32_t HashName(const std::wstring& name) {
    uint32_t h = 2166136261u;
    for (wchar_t ch : name) {
        h ^= static_cast<uint16_t>(ch);
        h *= 16777619u;
    }
    return h ? h : 1u;  // 0 names no terminal
}

struct PanelScan {
    void* cls;
    std::vector<void*>* out;
};

// The id -> panel cache: a placed panel keeps its id for the world's life, so a hit is validated and kept.
struct CachedPanel {
    uint32_t     id;
    CachedObjRef panel;
};
std::vector<CachedPanel> g_panelCache;
uint64_t g_nextPanelWalkMs = 0;

void RefreshPanelCache() {
    std::vector<void*> panels;
    EnumeratePanels(panels);
    g_panelCache.clear();
    for (void* p : panels) {
        const uint32_t id = PanelId(p);
        bool dup = false;
        for (const CachedPanel& c : g_panelCache)
            if (c.id == id) dup = true;
        if (dup) {
            UE_LOGW("sat_console: two panels share terminal id %08X ('%ls') -- the second is not a terminal", id,
                    PanelName(p).c_str());
            continue;
        }
        CachedPanel c;
        c.id = id;
        c.panel.Set(p);
        g_panelCache.push_back(std::move(c));
    }
}

}  // namespace

bool IsPanel(void* obj) {
    if (!obj || !R::IsLive(obj)) return false;
    void* cls = PanelClass();
    return cls && R::ClassOf(obj) == cls;
}

size_t EnumeratePanels(std::vector<void*>& out) {
    void* cls = PanelClass();
    if (!cls) return 0;
    const size_t before = out.size();
    PanelScan scan{cls, &out};
    object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t index) {
        auto* sc = static_cast<PanelScan*>(ctx);
        if (!obj || (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable))) return;
        if (R::ClassOf(obj) == sc->cls) sc->out->push_back(obj);
    }, &scan);
    return out.size() - before;
}

uint32_t PanelId(void* panel) {
    if (!IsPanel(panel)) return 0;
    return HashName(PanelName(panel));
}

std::wstring KeyForId(uint32_t id) {
    if (!id) return {};
    wchar_t key[16]{};
    swprintf_s(key, L"sat_%08x", id);
    return key;
}

std::wstring PanelKey(void* panel) { return KeyForId(PanelId(panel)); }

bool PanelContext(void* panel, TerminalBinding& out) {
    out = TerminalBinding{};
    if (!IsPanel(panel)) return false;
    out.panel = panel;
    out.id = PanelId(panel);
    out.key = KeyForId(out.id);
    if (!EnsurePanelResolved()) return true;  // a panel, its context not readable yet: not ready
    out.root = (*At<uint8_t>(panel, g_offPanelRoot) & g_maskPanelRoot) != 0;
    if (out.root) {
        out.name = L"ROOT";  // con()'s own literal
        out.ready = true;
        return true;
    }
    void* dish = ReadObject(panel, g_offPanelDish);
    if (!dish) return true;  // a field panel whose dish is not set yet: not ready, never ROOT
    out.dish = dish;
    out.name = ftext_utils::FTextToString(At<uint8_t>(dish, g_offDishName));
    out.ready = !out.name.empty();
    return true;
}

TerminalBinding TerminalForPanel(void* panel) {
    TerminalBinding b;
    PanelContext(panel, b);
    return b;
}

bool BindingForTerminal(uint32_t id, TerminalBinding& out) {
    out = TerminalBinding{};
    if (!id) return false;
    for (int pass = 0; pass < 2; ++pass) {
        for (CachedPanel& c : g_panelCache) {
            if (c.id != id) continue;
            void* p = c.panel.Get();
            if (p && IsPanel(p)) return PanelContext(p, out);
        }
        const uint64_t now = NowMs();
        if (pass == 1 || now < g_nextPanelWalkMs) break;
        g_nextPanelWalkMs = now + 500;
        RefreshPanelCache();
    }
    return false;
}

void* RootPanel() {
    if (!EnsurePanelResolved()) return nullptr;
    std::vector<void*> panels;
    EnumeratePanels(panels);
    for (void* p : panels)
        if (*At<uint8_t>(p, g_offPanelRoot) & g_maskPanelRoot) return p;
    return nullptr;
}

void* ReadOverlapActor(const uint8_t* locals) {
    if (!locals) return nullptr;
    EnsurePanelResolved();
    if (g_offOverlapActor < 0) return nullptr;
    void* a = *reinterpret_cast<void* const*>(locals + g_offOverlapActor);
    return (a && R::IsLive(a)) ? a : nullptr;
}

}  // namespace ue_wrap::sat_console

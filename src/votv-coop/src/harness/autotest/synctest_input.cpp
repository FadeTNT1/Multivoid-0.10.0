// The local harness must never take ownership of the desktop mouse or foreground window.
#include "harness/autotest/synctest.h"
#include "coop/config/config.h"
#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"
#include <windows.h>
#include <utility>
#include <vector>

namespace harness::autotest {
namespace {
decltype(&::ClipCursor) clipOriginal = nullptr;
decltype(&::ShowWindow) showOriginal = nullptr;
decltype(&::ShowWindowAsync) showAsyncOriginal = nullptr;
decltype(&::SetWindowPos) positionOriginal = nullptr;
decltype(&::CreateWindowExW) createOriginal = nullptr;
void* discarded[8]{};
BOOL WINAPI NoClip(const RECT*) { return TRUE; }
BOOL WINAPI NoCursor(int, int) { return TRUE; }
HWND WINAPI NoCapture(HWND) { return nullptr; }
BOOL WINAPI NoForeground(HWND) { return FALSE; }
HWND WINAPI NoFocus(HWND) { return nullptr; }
BOOL WINAPI NoRaise(HWND) { return TRUE; }
int WINAPI VisibleCursor(BOOL) { return 1; }
BOOL WINAPI ShowQuiet(HWND window, int command) { return showOriginal(window, command == SW_HIDE ? SW_HIDE : SW_SHOWMINNOACTIVE); }
BOOL WINAPI ShowAsyncQuiet(HWND window, int command) { return showAsyncOriginal(window, command == SW_HIDE ? SW_HIDE : SW_SHOWMINNOACTIVE); }
BOOL WINAPI PositionQuiet(HWND window, HWND after, int x, int y, int w, int h, UINT flags) {
    return positionOriginal(window, after, x, y, w, h, flags | SWP_NOACTIVATE);
}
HWND WINAPI CreateQuiet(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int w, int h,
                         HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    return createOriginal(ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, cls, title, style,
                          x, y, w, h, parent, menu, instance, param);
}
bool Same(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
}
bool SyncTestNoInputCapture() {
    static const bool armed = coop::config::ReadEnv("VOTVCOOP_SCENARIO") == "synctest" &&
        coop::config::ReadEnv("MULTIVOID_HARNESS_NO_INPUT_CAPTURE") == "1";
    return armed;
}
void InstallSyncTestInputGuard() {
    if (!SyncTestNoInputCapture()) return;
    HMODULE user = ::GetModuleHandleW(L"user32.dll");
    std::vector<std::pair<void*, void*>> entries;
    auto install = [user, &entries](const char* name, void* detour, void** trampoline) {
        void* target = user ? reinterpret_cast<void*>(::GetProcAddress(user, name)) : nullptr;
        // Some Windows builds export both cursor setters at the same entry point.
        for (const auto& entry : entries) if (entry.first == target) return entry.second == detour;
        if (!target || !ue_wrap::hook::Install(target, detour, trampoline)) return false;
        entries.emplace_back(target, detour);
        return true;
    };
    bool ok = ue_wrap::hook::Init();
    ok = install("ClipCursor", reinterpret_cast<void*>(&NoClip), reinterpret_cast<void**>(&clipOriginal)) && ok;
    ok = install("SetCursorPos", reinterpret_cast<void*>(&NoCursor), &discarded[0]) && ok;
    ok = install("SetPhysicalCursorPos", reinterpret_cast<void*>(&NoCursor), &discarded[1]) && ok;
    ok = install("SetCapture", reinterpret_cast<void*>(&NoCapture), &discarded[2]) && ok;
    ok = install("SetForegroundWindow", reinterpret_cast<void*>(&NoForeground), &discarded[3]) && ok;
    ok = install("SetFocus", reinterpret_cast<void*>(&NoFocus), &discarded[4]) && ok;
    ok = install("SetActiveWindow", reinterpret_cast<void*>(&NoFocus), &discarded[5]) && ok;
    ok = install("BringWindowToTop", reinterpret_cast<void*>(&NoRaise), &discarded[6]) && ok;
    ok = install("ShowCursor", reinterpret_cast<void*>(&VisibleCursor), &discarded[7]) && ok;
    ok = install("ShowWindow", reinterpret_cast<void*>(&ShowQuiet), reinterpret_cast<void**>(&showOriginal)) && ok;
    ok = install("ShowWindowAsync", reinterpret_cast<void*>(&ShowAsyncQuiet), reinterpret_cast<void**>(&showAsyncOriginal)) && ok;
    ok = install("SetWindowPos", reinterpret_cast<void*>(&PositionQuiet), reinterpret_cast<void**>(&positionOriginal)) && ok;
    ok = install("CreateWindowExW", reinterpret_cast<void*>(&CreateQuiet), reinterpret_cast<void**>(&createOriginal)) && ok;
    // A blocked clip and cursor warp must leave the desktop unchanged before any game code runs.
    RECT before{}, after{}, probeRect{0, 0, 1, 1}; POINT pos{}, next{};
    const bool read = ::GetClipCursor(&before) && ::GetCursorPos(&pos);
    if (ok) { ::ClipCursor(&probeRect); ::SetCursorPos(pos.x + 1, pos.y); }
    ok = ok && read && ::GetClipCursor(&after) && ::GetCursorPos(&next) && Same(before, after) &&
         pos.x == next.x && pos.y == next.y;
    UE_LOGI("synctest: INPUT GUARD %s (13 APIs; clipping, cursor warps, capture and activation blocked)", ok ? "PASS" : "FAIL");
    ue_wrap::log::Flush();
    if (!ok) ::TerminateProcess(::GetCurrentProcess(), 86);
}
}

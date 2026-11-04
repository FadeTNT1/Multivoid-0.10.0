// proxy_loader/loader.cpp -- the Multivoid loader: a dwmapi.dll proxy that starts the mod without UE4SS.
//
// WHY dwmapi. VotV-Win64-Shipping.exe imports four dwmapi functions (DwmFlush, DwmIsCompositionEnabled,
// DwmGetCompositionTimingInfo, DwmSetWindowAttribute), the Windows loader looks in the exe's own folder
// before System32, and dwmapi is not a KnownDLL: a dwmapi.dll dropped beside the exe is mapped in place of
// the system one. It is the same door UE4SS used; the full export set is forwarded (forward.cpp) so the
// game never notices.
//
// WHEN. start_mod must run where UE4SS ran it: on the game's main thread BEFORE the exe's entry point.
// bootstrap::PatchEngine patches the engine's ProcessEvent, script loop and EndPlay on start_mod's own call,
// and those patches are only safe before the engine runs (boot.cpp, PatchEngine). DllMain cannot load
// anything (loader lock), so it queues an APC on the current thread: the main thread, because the exe imports
// dwmapi statically. A thread's queued APCs are delivered when its process initialisation finishes -- after
// every DllMain has returned and the loader lock is released, before the exe's first instruction. That is the
// timing UE4SS's own proxy used, so the mod boots exactly as it did under it.
//
// SAFETY NET. If the APC never runs (this DLL loaded late by something else, on a thread that never waits
// alertably), a watchdog thread starts the mod after a grace period instead and logs that it did. One
// atomic latch decides which of the two does the work; the mod's own boot latch is a second guard, not the
// only one.
//
// STAND-ASIDE. If UE4SS is present in the process or beside the exe it loads the mod itself (Mods\Multivoid\
// dlls\main.dll, start_mod at its scan); this loader then does nothing at all.
//
// Everything is logged to <exe dir>\Mods\Multivoid\loader.log. This file imports nothing from UE4SS and
// nothing from the mod: main.dll is reached by LoadLibrary and the exported name start_mod only.

#include <windows.h>

#include <cstdarg>
#include <cwchar>
#include <string>

namespace {

constexpr wchar_t kModRel[] = L"\\Mods\\Multivoid";  // beside the exe
constexpr wchar_t kDllRel[] = L"\\Mods\\Multivoid\\dlls\\main.dll";
constexpr DWORD kGraceMs = 5000;  // the watchdog's wait for the APC
constexpr DWORD kLogRotateBytes = 256 * 1024;

constexpr LONG kIdle = 0;
constexpr LONG kRunning = 1;
volatile LONG g_latch = kIdle;
HANDLE g_apcDone = nullptr;  // signalled when the APC began (the watchdog then stands down)

std::wstring ExeDir() {
    std::wstring buf(32768, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    const size_t sep = buf.find_last_of(L"\\/");
    return sep == std::wstring::npos ? std::wstring{} : buf.substr(0, sep);
}

bool Exists(const std::wstring& p) { return ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// ---- the log ---------------------------------------------------------------------------------------------
// Opened per line: a few lines per process, so no handle is held and a crash loses nothing. Written only when
// Mods\Multivoid exists, so a stray dwmapi.dll in an unrelated folder leaves no litter.
std::wstring g_logPath;

void Log(const wchar_t* fmt, ...) {
    if (g_logPath.empty()) return;
    wchar_t msg[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
    va_end(ap);
    SYSTEMTIME t{};
    ::GetLocalTime(&t);
    wchar_t line[2200];
    const int n = _snwprintf_s(line, _TRUNCATE, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] [pid %lu] %ls\r\n", t.wYear,
                               t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                               ::GetCurrentProcessId(), msg);
    if (n <= 0) return;
    char utf8[8192];
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, line, n, utf8, sizeof(utf8), nullptr, nullptr);
    if (bytes <= 0) return;
    const HANDLE h = ::CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    ::WriteFile(h, utf8, static_cast<DWORD>(bytes), &wrote, nullptr);
    ::CloseHandle(h);
}

void OpenLog(const std::wstring& exeDir) {
    const std::wstring modDir = exeDir + kModRel;
    if (!Exists(modDir)) return;
    g_logPath = modDir + L"\\loader.log";
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (::GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &a) && a.nFileSizeHigh == 0 &&
        a.nFileSizeLow > kLogRotateBytes) {
        ::MoveFileExW(g_logPath.c_str(), (modDir + L"\\loader.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

// ---- the work ---------------------------------------------------------------------------------------------

// UE4SS beside the exe in any of its layouts (classic, lowercase, the ue4ss\ subfolder of newer builds) or
// already mapped into this process (a shim loader keeps it elsewhere).
bool Ue4ssPresent(const std::wstring& exeDir, std::wstring& why) {
    if (::GetModuleHandleW(L"UE4SS.dll") || ::GetModuleHandleW(L"ue4ss.dll")) {
        why = L"a UE4SS module is loaded in this process";
        return true;
    }
    static const wchar_t* const kRel[] = {L"\\UE4SS.dll", L"\\ue4ss.dll", L"\\ue4ss\\UE4SS.dll"};
    for (const wchar_t* rel : kRel) {
        if (Exists(exeDir + rel)) {
            why = exeDir + rel;
            return true;
        }
    }
    return false;
}

using StartModFn = void*(__cdecl*)();

// start_mod behind SEH: a fault in the mod's boot is logged here, with its code, and the game goes on without
// the mod instead of dying inside an APC with no trace. (No C++ objects in this frame: __try needs none.)
bool CallStartMod(StartModFn fn, void** result, DWORD* code) {
    __try {
        *result = fn();
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Run(const wchar_t* how) {
    const std::wstring exeDir = ExeDir();
    if (exeDir.empty()) return;
    OpenLog(exeDir);
    Log(L"loader: start via %ls", how);

    std::wstring why;
    if (Ue4ssPresent(exeDir, why)) {
        Log(L"loader: UE4SS present (%ls) -- standing aside, UE4SS starts the mod", why.c_str());
        return;
    }
    const std::wstring modDir = exeDir + kModRel;
    if (!Exists(modDir + L"\\enabled.txt")) {
        Log(L"loader: %ls\\enabled.txt missing -- the mod is disabled, nothing started", modDir.c_str());
        return;
    }
    const std::wstring dll = exeDir + kDllRel;
    if (!Exists(dll)) {
        Log(L"loader: FAIL %ls does not exist", dll.c_str());
        return;
    }
    // LOAD_WITH_ALTERED_SEARCH_PATH: main.dll's own folder is searched for anything it depends on.
    HMODULE mod = ::LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        Log(L"loader: FAIL LoadLibrary(%ls) gle=%lu", dll.c_str(), ::GetLastError());
        return;
    }
    Log(L"loader: main.dll mapped at %p", static_cast<void*>(mod));
    const auto start = reinterpret_cast<StartModFn>(::GetProcAddress(mod, "start_mod"));
    if (!start) {
        Log(L"loader: FAIL main.dll exports no start_mod (gle=%lu)", ::GetLastError());
        return;
    }
    void* handle = nullptr;
    DWORD code = 0;
    if (!CallStartMod(start, &handle, &code)) {
        Log(L"loader: FAIL start_mod faulted with exception 0x%08lX", code);
        return;
    }
    if (handle) {
        Log(L"loader: OK start_mod returned %p -- the mod started", handle);
    } else {
        Log(L"loader: start_mod returned null -- the mod refused to start (its own multivoid.log says why)");
    }
}

// Exactly one caller wins the latch and does the work.
void RunOnce(const wchar_t* how) {
    if (::InterlockedCompareExchange(&g_latch, kRunning, kIdle) != kIdle) return;
    Run(how);
}

VOID CALLBACK ApcProc(ULONG_PTR) {
    // Tell the watchdog first: the work below can take long (main.dll is large) and the watchdog must not
    // mistake a slow APC for a missing one.
    if (g_apcDone) ::SetEvent(g_apcDone);
    RunOnce(L"APC on the main thread");
}

DWORD WINAPI WatchdogThread(LPVOID) {
    if (::WaitForSingleObject(g_apcDone, kGraceMs) == WAIT_TIMEOUT) RunOnce(L"watchdog (the APC never ran)");
    return 0;
}

DWORD WINAPI PlainThread(LPVOID) {
    RunOnce(L"thread (the APC could not be queued)");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(module);
        // Nothing below loads a library or waits: only handle creation, an APC queue and a thread spawn.
        g_apcDone = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const bool queued = g_apcDone && ::QueueUserAPC(ApcProc, ::GetCurrentThread(), 0) != 0;
        // APC queued: a watchdog guards it. Not queued: a plain thread does the work (the loader starts it
        // only after this DllMain returns and the lock drops).
        if (HANDLE t = ::CreateThread(nullptr, 0, queued ? WatchdogThread : PlainThread, nullptr, 0, nullptr))
            ::CloseHandle(t);
    }
    return TRUE;
}

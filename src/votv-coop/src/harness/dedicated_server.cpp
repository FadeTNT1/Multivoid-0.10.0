// harness/dedicated_server.cpp -- see harness/dedicated_server.h. Every line this file logs carries
// the `[server]` tag; the ready line is a contract with the Pterodactyl egg (server-docker/).

#include "harness/dedicated_server.h"
#include "harness/dedicated_server_config.h"

#include "harness/session_runtime.h"

#include "coop/commands/command_sync.h"
#include "coop/comms/chat_sync.h"
#include "coop/net/end_reason.h"
#include "coop/net/master_slots.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // kMaxPeers
#include "coop/player/roster.h"
#include "coop/save/save_guard.h"
#include "coop/session/host_mode.h"
#include "coop/session/session_manager.h"
#include "coop/session/shutdown.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_save.h"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace harness::dedicated_server {
namespace {

namespace GT = ue_wrap::game_thread;
namespace fs = std::filesystem;

// The line a panel waits on (the egg's done-detection). Never reworded without the egg.
constexpr const char* kReadyLine = "Multivoid dedicated server ready";

std::atomic<bool> g_stopping{false};
std::atomic<bool> g_ready{false};

coop::net::Session& S() { return harness::session_runtime::Session(); }

bool HostRunning() { return S().running() && S().role() == coop::net::Role::Host; }

// ---- stdout ------------------------------------------------------------------------------------

HANDLE g_out = nullptr;
std::mutex g_outMu;

bool UsableHandle(HANDLE h) {
    return h && h != INVALID_HANDLE_VALUE && ::GetFileType(h) != FILE_TYPE_UNKNOWN;
}

// The log's one sink (ue_wrap/core/log.h): no logging in here, and its own lock.
void StdoutSink(ue_wrap::log::Level level, const char* msg) {
    if (!g_out || !msg) return;
    const char* tag = level == ue_wrap::log::Level::Error ? "[ERROR] "
                    : level == ue_wrap::log::Level::Warn  ? "[WARN] "
                                                          : "[INFO] ";
    std::string line = tag;
    line += msg;
    if (line.empty() || line.back() != '\n') line += '\n';
    std::lock_guard<std::mutex> lk(g_outMu);
    DWORD wrote = 0;
    ::WriteFile(g_out, line.data(), static_cast<DWORD>(line.size()), &wrote, nullptr);
}

void InstallStdoutMirror() {
    HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    // A GUI-subsystem exe started from a console inherits no handle; the parent's console is the
    // next best home (a Windows terminal, a panel's console host).
    if (!UsableHandle(h) && ::AttachConsole(ATTACH_PARENT_PROCESS)) h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (!UsableHandle(h)) {
        UE_LOGW("[server] no stdout handle -- the log is in %ls only",
                ue_wrap::log::CurrentPath().c_str());
        return;
    }
    g_out = h;
    ue_wrap::log::SetSink(&StdoutSink);
    UE_LOGI("[server] the log is mirrored to stdout");
}

// ---- the save ----------------------------------------------------------------------------------

// The game's own save on the game thread, waited for up to `timeoutMs`. False before the hosted world
// is up, when the save did not run, or when the wait ran out.
bool SaveBlocking(DWORD timeoutMs) {
    // Only once the hosted world has been up: a save during the boot's load would write a world
    // that is not built yet.
    if (!g_ready.load()) {
        UE_LOGI("[server] save skipped -- the hosted world is not up yet");
        return false;
    }
    auto st = std::make_shared<std::atomic<int>>(0);  // 0 pending, 1 saved, 2 failed
    GT::Post([st] { st->store(ue_wrap::world_save::SaveNow() ? 1 : 2); });
    const ULONGLONG t0 = ::GetTickCount64();
    while (st->load() == 0 && ::GetTickCount64() - t0 < timeoutMs) ::Sleep(20);
    const int r = st->load();
    if (r == 1) UE_LOGI("[server] world saved");
    else UE_LOGW("[server] the save %s", r == 2 ? "did not run" : "did not finish in time");
    return r == 1;
}

// ---- the stop ----------------------------------------------------------------------------------

struct StopRequest {
    int exitCode;
    std::string why;
};

// The stop sequence (docs/dedicated-server.md, "Graceful shutdown"). Exit code 0 asks the engine to
// quit as a window close would; any other code is a fault, and the process ends with it once the
// detours are lifted, so a panel's restart sees it.
DWORD WINAPI StopThread(LPVOID raw) {
    std::unique_ptr<StopRequest> req(static_cast<StopRequest*>(raw));
    UE_LOGI("[server] stopping (%s)", req->why.c_str());
    ue_wrap::log::Flush();
    if (!coop::shutdown::IsShuttingDown()) {
        SaveBlocking(60000);
        coop::session_manager::EndHostedLobby();  // a no-op when nothing was announced
        if (req->exitCode == 0) {
            auto posted = std::make_shared<std::atomic<bool>>(false);
            GT::Post([posted] {
                ue_wrap::engine::ExecuteConsoleCommand(L"quit");
                posted->store(true);
            });
            const ULONGLONG t0 = ::GetTickCount64();
            while (!posted->load() && ::GetTickCount64() - t0 < 10000) ::Sleep(20);
            if (!posted->load()) UE_LOGW("[server] the engine did not take the quit in 10 s");
        }
        coop::shutdown::DoShutdown();
    }
    UE_LOGI("[server] stopped");
    ue_wrap::log::Flush();
    if (req->exitCode != 0) ::ExitProcess(static_cast<UINT>(req->exitCode));
    // The engine's own exit normally ends the process long before this.
    ::Sleep(30000);
    UE_LOGW("[server] the engine has not exited 30 s after the quit -- ending the process");
    ue_wrap::log::Flush();
    ::ExitProcess(0);  // noreturn
}

void RequestStop(int exitCode, const char* why) {
    if (g_stopping.exchange(true)) return;
    auto* req = new StopRequest{exitCode, why ? why : ""};
    if (HANDLE t = ::CreateThread(nullptr, 0, StopThread, req, 0, nullptr)) {
        ::CloseHandle(t);
    } else {
        UE_LOGE("[server] the stop thread did not start -- ending the process");
        delete req;
        ue_wrap::log::Flush();
        ::ExitProcess(static_cast<UINT>(exitCode ? exitCode : 1));
    }
}

BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            RequestStop(0, "console interrupt");
            return TRUE;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            RequestStop(0, "console closed");
            // Windows ends the process when this returns; the save gets what time it allows.
            ::Sleep(4500);
            return TRUE;
        default:
            return FALSE;
    }
}

// ---- the commands ------------------------------------------------------------------------------

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void PrintPlayers() {
    coop::roster::Snapshot snap;
    coop::roster::GetSnapshot(snap);
    if (!snap.inSession) {
        UE_LOGI("[server] players: no session is running");
        return;
    }
    int shown = 0;
    for (int i = 0; i < coop::players::kMaxPeers; ++i) {
        const coop::roster::Row& r = snap.rows[i];
        if (!r.connected && !r.isLocal) continue;
        ++shown;
        UE_LOGI("[server]   slot %d  #%u  %s%s  ping %d ms  id %s", r.slot, static_cast<unsigned>(r.playerNo),
                r.nick, r.isLocal ? " (this server)" : "", r.ping, r.playerId[0] ? r.playerId : "-");
    }
    UE_LOGI("[server] players: %d connected, the server included (of %d)", shown, Get().maxPlayers);
}

DWORD WINAPI SaveThread(LPVOID) {
    SaveBlocking(60000);
    return 0;
}

void Help() {
    UE_LOGI("[server] commands: stop | save | say <message> | players | kick <name> | help | "
            "any chat command without its '/' (ban, unban, tphere, set, mv, time ...)");
}

void RunCommand(const std::string& raw) {
    std::string line = Trim(raw);
    if (!line.empty() && line[0] == '/') line = Trim(line.substr(1));
    if (line.empty()) return;
    const size_t sp = line.find(' ');
    std::string verb = line.substr(0, sp);
    for (char& c : verb) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string rest = sp == std::string::npos ? std::string() : Trim(line.substr(sp + 1));
    UE_LOGI("[server] > %s", line.c_str());

    if (verb == "stop" || verb == "quit" || verb == "exit") {
        RequestStop(0, "stop command");
    } else if (verb == "save") {
        // Off the reader thread: a save takes seconds and the next line should still be read.
        if (HANDLE t = ::CreateThread(nullptr, 0, SaveThread, nullptr, 0, nullptr)) ::CloseHandle(t);
    } else if (verb == "say") {
        if (rest.empty()) { UE_LOGI("[server] say: nothing to say"); return; }
        if (!HostRunning()) { UE_LOGI("[server] say: no session is running"); return; }
        coop::chat_sync::QueueSend(rest);
    } else if (verb == "players" || verb == "list") {
        PrintPlayers();
    } else if (verb == "help" || verb == "?") {
        Help();
    } else {
        // `kick <name>` and every other chat command run as the host operator; the reply is logged
        // by command_sync ("command_sync: reply ...").
        coop::command_sync::Submit(line);
    }
}

// Lines split from a byte stream that may hand them over in pieces.
struct LineSplitter {
    std::string pending;
    void Feed(const char* data, size_t n) {
        pending.append(data, n);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            const std::string one = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!g_stopping.load()) RunCommand(one);
        }
    }
};

DWORD WINAPI StdinThread(LPVOID) {
    HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
    if (!UsableHandle(in)) {
        UE_LOGI("[server] no stdin -- commands are read from server_commands.txt only");
        return 0;
    }
    UE_LOGI("[server] reading commands from stdin");
    LineSplitter split;
    char buf[1024];
    while (!coop::shutdown::IsShuttingDown()) {
        DWORD got = 0;
        if (!::ReadFile(in, buf, sizeof(buf), &got, nullptr) || got == 0) break;  // EOF: the file stays
        split.Feed(buf, got);
    }
    UE_LOGI("[server] stdin closed -- server_commands.txt is still read");
    return 0;
}

std::wstring CommandFile() {
    const std::wstring dir = ue_wrap::paths::ModDataDir();
    return dir.empty() ? std::wstring() : dir + L"\\server_commands.txt";
}

DWORD WINAPI CommandFileThread(LPVOID) {
    const std::wstring path = CommandFile();
    if (path.empty()) {
        UE_LOGW("[server] no data folder -- server_commands.txt is not read");
        return 0;
    }
    const std::wstring taken = path + L".reading";
    // A stop left from the last run must not end this one.
    ::DeleteFileW(path.c_str());
    ::DeleteFileW(taken.c_str());
    UE_LOGI("[server] reading commands from %ls", path.c_str());
    while (!coop::shutdown::IsShuttingDown()) {
        ::Sleep(1000);
        // Taken by a rename, so a line appended while this reads lands in a new file, not lost.
        if (!::MoveFileExW(path.c_str(), taken.c_str(), MOVEFILE_REPLACE_EXISTING)) continue;
        std::string bytes;
        FILE* f = nullptr;
        if (::_wfopen_s(&f, taken.c_str(), L"rb") == 0 && f) {
            char buf[1024];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.append(buf, n);
            std::fclose(f);
        }
        ::DeleteFileW(taken.c_str());
        LineSplitter split;
        split.Feed(bytes.data(), bytes.size());
        if (!split.pending.empty() && !g_stopping.load()) RunCommand(split.pending);  // no final newline
    }
    return 0;
}

// ---- the driver --------------------------------------------------------------------------------

// The world to host: an existing world folder `<slot>` (SaveGames/<slot>/main.meta), or a new
// story world named `<slot>`.
coop::session_manager::SaveChoice ResolveSave(const Settings& s) {
    coop::session_manager::SaveChoice c;
    const fs::path dir = coop::save_guard::SaveGamesDir();
    std::error_code ec;
    const std::wstring wslot(s.saveSlot.begin(), s.saveSlot.end());  // ASCII world names
    if (!dir.empty() && fs::is_regular_file(dir / wslot / L"main.meta", ec)) {
        c.slot = s.saveSlot;
    } else {
        c.newGame = true;
        c.newName = s.saveSlot;
        c.mode = 0;  // story
        c.nameIsDerived = false;  // the owner named it: the exact name or a refusal
    }
    return c;
}

bool HostFailed() {
    const std::string st = coop::session_manager::HostStatus();
    return st.rfind("Host failed", 0) == 0;
}

DWORD WINAPI DriverThread(LPVOID) {
    const Settings& s = Get();
    ::Sleep(2000);  // the menu scenario's own settle: the master list and the identity are in
    if (coop::shutdown::IsShuttingDown()) return 0;

    const coop::session_manager::SaveChoice choice = ResolveSave(s);
    if (choice.newGame)
        UE_LOGI("[server] no world '%s' -- creating a new story world of that name", s.saveSlot.c_str());
    else
        UE_LOGI("[server] hosting the world '%s'", choice.slot.c_str());

    coop::session::HostMode mode;
    mode.reach = s.brokered ? coop::session::Reachability::Brokered : coop::session::Reachability::Direct;
    mode.listed = s.listPublic;
    const bool locked = !s.password.empty();

    bool queued = false;
    for (int attempt = 0; attempt < 30 && !queued && !coop::shutdown::IsShuttingDown(); ++attempt) {
        coop::net::Refusal refused;
        queued = coop::session_manager::HostWithSave(choice, s.name, locked, s.password, s.maxPlayers,
                                                     coop::net::master_slots::Selected().url, mode,
                                                     &refused);
        if (queued) break;
        if (refused.code != coop::net::EndReason::None) {
            UE_LOGE("[server] this build refuses to host [%s]: %s", coop::net::Describe(refused.code).id,
                    refused.detail.c_str());
            RequestStop(4, "the host was refused");
            return 0;
        }
        ::Sleep(2000);  // another action in flight; it ends by itself
    }
    if (!queued) {
        UE_LOGE("[server] the host could not be queued");
        RequestStop(3, "the host could not be queued");
        return 0;
    }

    const ULONGLONG t0 = ::GetTickCount64();
    while (!HostRunning() && !coop::shutdown::IsShuttingDown() && !g_stopping.load()) {
        if (HostFailed()) {
            UE_LOGE("[server] %s", coop::session_manager::HostStatus().c_str());
            RequestStop(3, "the host failed");
            return 0;
        }
        if (::GetTickCount64() - t0 > static_cast<ULONGLONG>(s.readyTimeoutS) * 1000ull) {
            UE_LOGE("[server] the host was not up %d s after it was queued", s.readyTimeoutS);
            RequestStop(3, "the host did not come up in time");
            return 0;
        }
        ::Sleep(250);
    }
    if (!HostRunning()) return 0;

    if (s.maxFps > 0) {
        const std::wstring cmd = L"t.MaxFPS " + std::to_wstring(s.maxFps);
        GT::Post([cmd] { ue_wrap::engine::ExecuteConsoleCommand(cmd.c_str()); });
    }
    g_ready.store(true);
    UE_LOGI("[server] %s -- %s", kReadyLine, Describe(s).c_str());
    ue_wrap::log::Flush();

    ULONGLONG lastSave = ::GetTickCount64();
    while (!coop::shutdown::IsShuttingDown() && !g_stopping.load()) {
        ::Sleep(1000);
        if (g_stopping.load()) break;
        if (!HostRunning()) {
            UE_LOGE("[server] the host session ended by itself (%s)",
                    coop::session_manager::HostStatus().c_str());
            RequestStop(2, "the host session ended");
            break;
        }
        if (s.autosaveMinutes > 0 &&
            ::GetTickCount64() - lastSave >= static_cast<ULONGLONG>(s.autosaveMinutes) * 60000ull) {
            UE_LOGI("[server] autosave");
            SaveBlocking(60000);
            lastSave = ::GetTickCount64();
        }
    }
    return 0;
}

void Spawn(LPTHREAD_START_ROUTINE fn, const char* what) {
    if (HANDLE t = ::CreateThread(nullptr, 0, fn, nullptr, 0, nullptr)) ::CloseHandle(t);
    else UE_LOGE("[server] the %s thread did not start", what);
}

}  // namespace

bool ApplyBootOverrides() {
    if (!Enabled()) return false;
    const Settings& s = Get();
    if (s.stdoutMirror) InstallStdoutMirror();
    UE_LOGI("[server] dedicated server mode -- %s", Describe(s).c_str());
    // The rows the server sets, through their env twins (env beats ini): read by every config
    // lookup from here on, and nothing is written to the player's multivoid.ini.
    if (s.port) ::SetEnvironmentVariableA("VOTVCOOP_NET_PORT", std::to_string(s.port).c_str());
    ::SetEnvironmentVariableA("VOTVCOOP_NET_NICK", s.hostNick.c_str());
    // The server is the shipped menu launch with no person at it: no test scenario.
    char scenario[32] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_SCENARIO", scenario, sizeof(scenario)) > 0 &&
        std::string(scenario) != "menu")
        UE_LOGW("[server] VOTVCOOP_SCENARIO=%s is ignored -- a server runs the menu scenario", scenario);
    ::SetEnvironmentVariableA("VOTVCOOP_SCENARIO", "menu");
    if (!::SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE))
        UE_LOGW("[server] no console control handler (gle=%lu) -- stop with the stop command",
                ::GetLastError());
    return true;
}

bool SkipInteractiveSurfaces() { return Enabled(); }

void Launch() {
    if (!Enabled()) return;
    static std::atomic<bool> s_launched{false};
    if (s_launched.exchange(true)) return;
    Help();
    Spawn(CommandFileThread, "command file");
    Spawn(StdinThread, "stdin");
    Spawn(DriverThread, "driver");
}

}  // namespace harness::dedicated_server

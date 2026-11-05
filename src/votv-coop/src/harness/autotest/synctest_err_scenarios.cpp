// Adversarial event lifecycle scenarios. All engine reads/writes run on the game thread.
// The base alarm calls lib_C::setEvent, so this exercises a native in-flight event without
// spawning hostiles. The race reuses the container drill and its existing conservation verdict.

#include "synctest_err_scenarios.h"

#include "harness/autotest.h"
#include "harness/session_runtime.h"
#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "coop/session/session_manager.h"
#include "coop/session/shutdown.h"
#include "coop/world/alarm_sync.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine_save.h"
#include "ue_wrap/engine/save_capture.h"
#include "ue_wrap/engine/world_folder.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {
namespace GT = ue_wrap::game_thread;
namespace WI = ue_wrap::world_identity;
namespace Alarm = coop::alarm_sync;
namespace cfg = coop::config;
namespace fs = std::filesystem;
constexpr ULONGLONG kBudgetMs = 240000;

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    std::replace(value.begin(), value.end(), L'/', L'\\');
    return value;
}

bool Step(std::function<bool()> body) {
    return GT::RunAndWait([body](std::atomic<int>& done) { done.store(body() ? 1 : 2); }) == 1;
}

bool Until(const std::function<bool()>& condition) {
    const auto start = ::GetTickCount64();
    do {
        if (coop::shutdown::IsShuttingDown()) return false;
        if (condition()) return true;
        ::Sleep(250);
    } while (::GetTickCount64() - start < kBudgetMs);
    return false;
}

bool Ready() {
    return Step([] {
        const auto& session = harness::session_runtime::Session();
        return session.running() && (session.role() == coop::net::Role::Host || session.connected()) &&
               WI::CurrentWorldKind() == WI::WorldKind::Gameplay &&
               coop::players::Registry::Get().Local() != nullptr;
    });
}

bool Active(bool want) {
    return Step([want] {
        bool active = false;
        return Alarm::ReadLocalActive(active) && active == want;
    });
}

struct Run {
    std::string scenario;
    fs::path dir;
    bool client = IsClientRole();

    bool Write(const char* name) const {
        std::ofstream file(dir / name, std::ios::binary | std::ios::trunc);
        file << scenario << '\n';
        file.close();
        return !file.fail();
    }
    bool Has(const char* name) const {
        std::ifstream file(dir / name);
        std::string value;
        return (file >> value) && value == scenario;
    }
    bool Wait(const char* name) const { return Until([&] { return Has(name); }); }
    void Verdict(bool pass, const char* why) const {
        if (pass) UE_LOGI("synctest_err: VERDICT PASS scenario=%s role=%s -- %s", scenario.c_str(), client ? "client" : "host", why);
        else UE_LOGE("synctest_err: VERDICT FAIL scenario=%s role=%s -- %s", scenario.c_str(), client ? "client" : "host", why);
        ue_wrap::log::Flush();
    }
};

bool Guard(const Run& run) {
    wchar_t exe[32768]{};
    if (!::GetModuleFileNameW(nullptr, exe, 32768)) return false;
    if (Lower(exe).find(L"\\votv 0.10.0\\rig\\") == std::wstring::npos) return false;
    if (run.dir.empty() || !run.dir.is_absolute()) return false;
    std::error_code error;
    if (!fs::is_directory(run.dir, error)) return false;
    // The engine's path honours -saveddirsuffix; env alone is not evidence of save isolation.
    return Step([] {
        const auto saves = Lower(ue_wrap::world_folder::SaveGamesDir());
        wchar_t local[32768]{};
        if (!::GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768)) return false;
        auto original = Lower(std::wstring(local) + L"\\VotV\\Saved\\SaveGames\\");
        if (saves.empty() || saves.find(original) == 0) return false;
        const auto slot = cfg::ReadEnv("VOTVCOOP_SAVE");
        return saves.find(L"synctest_") != std::wstring::npos &&
               slot.rfind("synctest_", 0) == 0 && slot.find_first_of("/\\:") == std::string::npos;
    });
}

bool LeaveToMenu() {
    // Stop joins the network thread here; teardown and travel then run on the engine thread.
    auto& session = harness::session_runtime::Session();
    session.Stop();
    if (!Step([] {
        coop::net_pump::TearDownSession(harness::session_runtime::Session(), "synctest_err intentional leave");
        if (WI::CurrentWorldKind() == WI::WorldKind::Other) return true;
        GT::SetTransparentBypass(3000);
        return ue_wrap::engine::ReturnToMainMenu();
    })) return false;
    return Until([] { return Step([] { return WI::CurrentWorldKind() == WI::WorldKind::Other; }); });
}

bool StartEvent(const Run& run) {
    Alarm::DevForce(false);
    if (!Until([] { return Active(false); })) return false;
    Alarm::DevForce(true);
    if (!Until([] {
        return Active(true) && Step([] {
            return ue_wrap::save_capture::GameEventState() == ue_wrap::save_capture::EventState::On;
        });
    })) return false;
    UE_LOGI("synctest_err: EVENT_ACTIVE scenario=%s -- native alarm and getEvent=On", run.scenario.c_str());
    ue_wrap::log::Flush();
    return run.Write("event-on");
}

bool ReloadHost(const Run& run) {
    auto slot = std::make_shared<std::wstring>(L"synctest_err_reload_" + std::to_wstring(::GetCurrentProcessId()));
    auto oldWorld = std::make_shared<void*>(nullptr);
    if (!Step([slot, oldWorld] {
        *oldWorld = WI::CurrentWorld();
        return ue_wrap::save_capture::GameEventState() == ue_wrap::save_capture::EventState::On &&
               ue_wrap::save_capture::CaptureLiveWorldToScratchSlot(*slot);
    })) return false;
    if (!run.Write("host-saved") || !LeaveToMenu()) return false;
    if (!Step([] { ue_wrap::engine::ResetCachedSave(); return true; })) return false;
    if (!Until([slot, oldWorld] {
        return Step([slot, oldWorld] {
            const bool loaded = ue_wrap::engine::LoadStorySave(slot->c_str(), 0);
            return loaded && WI::CurrentWorldKind() == WI::WorldKind::Gameplay &&
                   WI::CurrentWorld() != *oldWorld;
        });
    })) return false;
    // Resolve the local trigger even though ending the host session cleared the lane's cache.
    // Do not force the alarm back on: this read is the save/load assertion.
    const bool restored = Until([] { return Active(true); });
    UE_LOGI("synctest_err: RELOADED event-active=%d -- session ended intentionally; no automatic rehost", restored ? 1 : 0);
    return restored && run.Write("host-reloaded");
}

bool Host(const Run& run) {
    auto& session = harness::session_runtime::Session();
    if (run.scenario != "join" && !WaitPeerWorldReady(1, static_cast<DWORD>(kBudgetMs))) return false;
    if (!StartEvent(run)) return false;
    if (run.scenario == "race") {
        if (!run.Wait("client-on")) return false;
        coop::director::RunContainerRace();
        UE_LOGI("synctest_err: STAGED scenario=race role=host -- read director/ctake-race RESULT verdict; no wrapper PASS");
        Alarm::DevForce(false);
        return true;
    }
    if (!run.Wait("client-on")) return false;
    if (run.scenario == "reload") return ReloadHost(run) && run.Wait("client-left");
    if (run.scenario == "rejoin" || run.scenario == "disconnect") {
        if (!Until([&] { return !session.IsSlotReady(1); })) return false;
        if (!session.running() || !Active(true) || !run.Write("host-peer-left")) return false;
        if (run.scenario == "rejoin") {
            if (!WaitPeerWorldReady(1, static_cast<DWORD>(kBudgetMs)) || !run.Wait("client-rejoined-on")) return false;
        } else {
            // A complete event poll after departure must preserve the host's event and session.
            ::Sleep(3000);
            if (!session.running() || !Active(true) || !run.Wait("client-left")) return false;
        }
    }
    Alarm::DevForce(false);
    if (!Until([] { return Active(false); }) || !run.Write("event-off")) return false;
    return run.scenario == "disconnect" || run.Wait("client-off");
}

bool Client(const Run& run) {
    if (!run.Wait("event-on") || !Until([] { return Ready() && Active(true); }) || !run.Write("client-on")) return false;
    UE_LOGI("synctest_err: CLIENT_APPLIED scenario=%s active=1", run.scenario.c_str());
    if (run.scenario == "race") {
        coop::director::RunContainerRace();
        UE_LOGI("synctest_err: STAGED scenario=race role=client -- read director/ctake-race RESULT verdict; no wrapper PASS");
        return true;
    }
    if (run.scenario == "reload") {
        return run.Wait("host-saved") && Until([] { return !harness::session_runtime::Session().running(); }) &&
               Until([] { return Step([] { return WI::CurrentWorldKind() == WI::WorldKind::Other; }); }) &&
               run.Write("client-left") && run.Wait("host-reloaded");
    }
    if (run.scenario == "rejoin" || run.scenario == "disconnect") {
        if (!LeaveToMenu() || !run.Write("client-left") || !run.Wait("host-peer-left")) return false;
        if (run.scenario == "disconnect") return true;
        const auto peer = cfg::ReadEnv("VOTVCOOP_NET_PEER");
        const auto port = cfg::ReadEnv("VOTVCOOP_NET_PORT");
        if (peer != "127.0.0.1" || port.empty()) return false;
        if (!coop::session_manager::ConnectDirect(peer + ":" + port) ||
            !Until([] { return Ready() && Active(true); }) || !run.Write("client-rejoined-on")) return false;
        UE_LOGI("synctest_err: CLIENT_REJOIN_APPLIED active=1");
    }
    return run.Wait("event-off") && Until([] { return Active(false); }) && run.Write("client-off");
}

DWORD WINAPI Worker(LPVOID) {
    const Run run{cfg::ReadEnv("VOTVCOOP_SYNCTEST_ERR"), fs::path(cfg::ReadEnv("VOTVCOOP_SYNCTEST_ERR_DIR"))};
    if (!Until([] { return Ready(); }) || !Guard(run)) {
        run.Verdict(false, "requires rig executable, isolated synctest_ save directory/world, and existing absolute barrier directory");
        return 0;
    }
    // Each peer owns one marker; stale directories fail closed rather than reusing old evidence.
    const char* started = run.client ? "client-started" : "host-started";
    const auto claim = run.dir / started;
    HANDLE file = ::CreateFileW(claim.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { run.Verdict(false, "barrier directory reused or unavailable"); return 0; }
    ::CloseHandle(file);
    const bool pass = run.client ? Client(run) : Host(run);
    if (run.scenario != "race" || !pass) run.Verdict(pass, "native event lifecycle and peer milestones checked");
    return 0;
}
}  // namespace

void SpawnSyncTestErrors() {
    const auto scenario = cfg::ReadEnv("VOTVCOOP_SYNCTEST_ERR");
    if (scenario.empty()) return;
    if (scenario != "join" && scenario != "rejoin" && scenario != "reload" && scenario != "race" && scenario != "disconnect") {
        UE_LOGE("synctest_err: VERDICT FAIL -- unknown VOTVCOOP_SYNCTEST_ERR=%s", scenario.c_str());
        return;
    }
    if (scenario == "race" && (cfg::ReadEnv("VOTVCOOP_RACE_GO_FILE").empty() ||
        cfg::ReadEnv("VOTVCOOP_RACE_ROLE") != (IsClientRole() ? "client" : "host") ||
        (cfg::ReadEnv("VOTVCOOP_RACE_MODE") != "race" && cfg::ReadEnv("VOTVCOOP_RACE_MODE") != "control"))) {
        UE_LOGE("synctest_err: VERDICT FAIL scenario=race -- requires GO_FILE, matching RACE_ROLE, and RACE_MODE=race|control");
        return;
    }
    UE_LOGI("synctest_err: ARMED scenario=%s role=%s", scenario.c_str(), IsClientRole() ? "client" : "host");
    if (HANDLE thread = ::CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) ::CloseHandle(thread);
    else UE_LOGE("synctest_err: VERDICT FAIL -- worker could not start");
}
}  // namespace harness::autotest

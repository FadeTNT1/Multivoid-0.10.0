// Local two-peer event driver. All engine work runs on the game thread; commands are local files.
#include "harness/autotest/synctest.h"
#include "harness/autotest.h"
#include "harness/session_runtime.h"
#include "harness/world_boot.h"
#include "coop/config/config.h"
#include "coop/dev/event_trigger.h"
#include "coop/net/session.h"
#include "coop/session/session_manager.h"
#include "coop/world/weather_rain.h"
#include "coop/world/weather_fog.h"
#include "coop/world/alarm_sync.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_folder.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/skysphere.h"
#include "ue_wrap/world/red_sky.h"
#include "ue_wrap/world/bad_sun.h"
#include "ue_wrap/world/jellyfish_path.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/world/directionalwind.h"
#include "ue_wrap/devices/atv.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace cfg = coop::config;

bool Extra(const std::string& name) {
    if (name == "alarm ON" || name == "alarm OFF") {
        coop::alarm_sync::DevForce(name == "alarm ON");
        return true;
    }
    if (name == "snow ON" || name == "snow OFF")
        return coop::weather_rain::DebugForceSnow(name == "snow ON");
    if (name == "sky.eye ON" || name == "sky.eye OFF")
        return ue_wrap::skysphere::CallSetEye(name == "sky.eye ON");
    if (name == "sky.blackhole ON" || name == "sky.blackhole OFF")
        return ue_wrap::skysphere::ApplyBlackHole(name == "sky.blackhole ON");
    if (name == "sky.rotation+moon") {
        ue_wrap::FRotator rot{};
        float phase = 0;
        if (!ue_wrap::skysphere::ReadSky(rot, phase)) return false;
        rot.Yaw += 30.f;
        ue_wrap::skysphere::ApplySky(rot, 0.375f);
        return true;
    }
    if (name == "jellyfish") return ue_wrap::jellyfish_path::CallSpawn();
    if (name == "fireflies") {
        void* spawner = ue_wrap::world_singleton::Find(L"ticker_fireflySpawner_C");
        void* fn = spawner ? R::FindFunction(R::ClassOf(spawner), L"ReceiveTick") : nullptr;
        if (!fn) return false;
        ue_wrap::ParamFrame frame(fn);
        frame.Set<float>(L"DeltaSeconds", 30.f);
        return ue_wrap::Call(spawner, frame);
    }
    if (name == "wind") {
        void* wind = ue_wrap::world_singleton::Find(ue_wrap::profile::name::DirectionalWindClass);
        void* fn = wind ? R::FindFunction(R::ClassOf(wind), L"changeWindOrigin") : nullptr;
        if (!fn) return false;
        ue_wrap::ParamFrame frame(fn);
        return ue_wrap::Call(wind, frame);
    }
    if (name == "lightning") {
        void* cls = R::FindClass(L"lightningStrike_C");
        return cls && ue_wrap::engine::SpawnActor(cls, {0.f, 0.f, 10000.f});
    }
    // Read-only baseline for documented lanes with no deterministic trigger in this driver.
    return false;
}

void Sample(const std::string& id, const std::string& name) {
    coop::net::WeatherStatePayload weather{};
    void* cycle = coop::weather_rain::Cycle();
    if (cycle) {
        coop::weather_rain::ReadState(cycle, weather);
        coop::weather_fog::ReadHostFogState(cycle, weather);
        // The production black-fog read is host-only; the probe measures actual presence on either role.
        weather.flags2 &= ~coop::net::fog_flags2::kBlackFogActive;
        if (ue_wrap::world_singleton::Find(ue_wrap::profile::name::BlackFogClass))
            weather.flags2 |= coop::net::fog_flags2::kBlackFogActive;
    }
    ue_wrap::FRotator sky{};
    float moon = -1.f;
    const bool skyOk = ue_wrap::skysphere::ReadSky(sky, moon);
    bool eye = false, hole = false, red = false, sun = false;
    const bool eyeOk = ue_wrap::skysphere::ReadEye(eye);
    const bool holeOk = ue_wrap::skysphere::ReadBlackHole(hole);
    const bool redOk = ue_wrap::red_sky::Read(nullptr, red);
    const bool sunOk = ue_wrap::bad_sun::ReadGamemodeCopy(nullptr, sun);
    ue_wrap::FVector wind{};
    const bool windOk = ue_wrap::directionalwind::ReadTarget(wind);
    // The gamemode's car, the ATV the atvExplode special arms: its trap bit (-1: no car or no read).
    int trap = -1;
    if (void* gm = ue_wrap::world_singleton::Gamemode()) {
        static int32_t carOff = -1;
        if (carOff < 0) carOff = R::FindPropertyOffset(R::ClassOf(gm), ue_wrap::profile::name::GamemodeCarProp);
        void* car = carOff < 0 ? nullptr : *reinterpret_cast<void**>(static_cast<uint8_t*>(gm) + carOff);
        bool armed = false;
        if (car && R::IsLive(car) && ue_wrap::atv::ReadTrap(car, armed)) trap = armed ? 1 : 0;
    }
    UE_LOGI("synctest: STATE id=%s name=%s weather=%d flags=%u fog=%u rain=%.3f sky=%d yaw=%.2f moon=%.3f eye=%d hole=%d red=%d sun=%d fish=%d wind=%d wx=%.1f wy=%.1f wz=%.1f trap=%d",
            id.c_str(), name.c_str(), cycle ? 1 : 0, weather.flags, weather.flags2, weather.rainStrength,
            skyOk ? 1 : 0, sky.Yaw, moon, eyeOk ? int(eye) : -1, holeOk ? int(hole) : -1,
            redOk ? int(red) : -1, sunOk ? int(sun) : -1,
            ue_wrap::jellyfish_path::ForEachFish(nullptr, nullptr), windOk ? 1 : 0, wind.X, wind.Y, wind.Z, trap);
}

DWORD WINAPI Driver(LPVOID) {
    const std::filesystem::path command(cfg::ReadEnv("VOTVCOOP_SYNCTEST_COMMAND"));
    if (command.empty()) return 0;
    auto& session = session_runtime::Session();
    for (int i = 0; i < 600 && !session.running(); ++i) ::Sleep(500);
    if (!session.running()) return 0;
    if (!IsClientRole() && !WaitPeerWorldReady(1, 300'000)) {
        UE_LOGE("synctest: FAIL client world-ready timeout");
        ue_wrap::log::Flush();
        return 0;
    }
    UE_LOGI("synctest: DRIVER READY role=%s", IsClientRole() ? "client" : "host");
    ue_wrap::log::Flush();
    std::string previous, id, name;
    while (session.running()) {
        std::ifstream input(command);
        std::string line;
        std::getline(input, line);
        input.close();
        const auto sep = line.find('|');
        if (sep != std::string::npos && line != previous) {
            previous = line;
            id = line.substr(0, sep);
            name = line.substr(sep + 1);
            UE_LOGI("synctest: BEGIN id=%s name=%s", id.c_str(), name.c_str());
            if (!IsClientRole()) {
                bool found = false;
                for (const auto& ev : coop::dev::event_trigger::Events()) {
                    if (name != ev.name) continue;
                    found = true;
                    const bool queued = coop::dev::event_trigger::Trigger(ev);
                    UE_LOGI("synctest: TRIGGER id=%s name=%s queued=%d", id.c_str(), name.c_str(), int(queued));
                    break;
                }
                if (!found) GT::Post([id, name] {
                    const bool ok = Extra(name);
                    UE_LOGI("synctest: EXTRA id=%s name=%s dispatched=%d", id.c_str(), name.c_str(), int(ok));
                });
            }
        }
        if (!id.empty()) GT::Post([id, name] {
            if (!IsClientRole() && name == "fireflies") Extra(name);
            Sample(id, name);
            ue_wrap::log::Flush();
        });
        ue_wrap::log::Flush();
        ::Sleep(1000);
    }
    return 0;
}
}

void RunSyncTestScenario() {
    if (cfg::ReadEnv("VOTVCOOP_SCENARIO") != "synctest") return;
    bool enabled = false;
    auto config = cfg::ReadNetConfig(enabled);
    if (!enabled || config.topology != coop::net::Topology::LanDirect || config.peerIp != "127.0.0.1") {
        UE_LOGE("synctest: REFUSED non-loopback or non-direct configuration");
        return;
    }
    // The disclaimer's automatic travel must finish before a session can exist.
    auto atMenu = std::make_shared<std::atomic<bool>>(false);
    for (int i = 0; i < 90 && !atMenu->load(); ++i) {
        GT::Post([atMenu] {
            void* world = ue_wrap::world_identity::CurrentWorld();
            atMenu->store(world && R::IsLive(world) && R::ToString(R::NameOf(world)) == L"menu");
        });
        ::Sleep(1000);
    }
    if (!atMenu->load()) { UE_LOGE("synctest: FAIL native main menu never settled"); return; }
    UE_LOGI("synctest: native main menu ready before session boot");
    // Fail closed before any world boot if the engine ignored the private user directory.
    auto isolated = std::make_shared<std::atomic<int>>(0);
    GT::Post([isolated] {
        const auto path = ue_wrap::world_folder::SaveGamesDir();
        auto normalize = [](std::filesystem::path p) {
            auto value = p.lexically_normal().wstring();
            std::replace(value.begin(), value.end(), L'/', L'\\');
            while (!value.empty() && value.back() == L'\\') value.pop_back();
            return value;
        };
        const auto expected = normalize(std::filesystem::path(cfg::ReadEnv("LOCALAPPDATA")) / L"VotV/Saved/SaveGames");
        const bool ok = path.find(L"synctest_") != std::wstring::npos &&
                        path.find(L"rig") != std::wstring::npos &&
                        _wcsicmp(normalize(path).c_str(), expected.c_str()) == 0;
        UE_LOGI("synctest: SAVE ISOLATION %s path=%ls", ok ? "PASS" : "FAIL", path.c_str());
        if (!ok) UE_LOGE("synctest: expected private SaveGames at %ls", expected.c_str());
        isolated->store(ok ? 1 : -1);
        ue_wrap::log::Flush();
    });
    for (int i = 0; i < 600 && isolated->load() == 0; ++i) ::Sleep(100);
    if (isolated->load() != 1) return;
    const bool host = config.role == coop::net::Role::Host;
    if (host) {
        if (!world_boot::BootStorySaveBlocking(true)) return;
        // Name the blank world without saving or loading any existing slot.
        auto named = std::make_shared<std::atomic<bool>>(false);
        for (int i = 0; i < 30 && !named->load(); ++i) {
            GT::Post([named] {
                void* gi = ue_wrap::world_singleton::GameInstance();
                if (!gi) return;
                void* fn = R::FindFunction(R::ClassOf(gi), ue_wrap::profile::name::SetSaveSlotObjectFn);
                if (!fn) return;
                void* save = ue_wrap::world_singleton::SaveSlot();
                if (!save) return;
                std::wstring slot = L"synctest_world", folder;
                R::FString s{slot.data(), int32_t(slot.size() + 1), int32_t(slot.size() + 1)};
                R::FString f{folder.data(), 1, 1};
                ue_wrap::ParamFrame frame(fn);
                frame.Set<void*>(L"save_gameInst", save);
                frame.SetRaw(L"SlotName", &s, sizeof(s));
                frame.SetRaw(L"folderSlotName", &f, sizeof(f));
                frame.Set<void*>(L"metadataObject", nullptr);
                frame.Set<void*>(L"save_persistent", nullptr);
                if (ue_wrap::Call(gi, frame)) {
                    named->store(true);
                    UE_LOGI("synctest: blank world registered as synctest_world");
                } else UE_LOGE("synctest: blank world name registration failed");
            });
            ::Sleep(1000);
        }
        if (!named->load()) { UE_LOGE("synctest: FAIL test world could not be named"); return; }
        session_runtime::StartCoopSession(config);
    } else {
        const std::string address = "127.0.0.1:" + std::to_string(config.port);
        if (!coop::session_manager::ConnectDirect(address.c_str())) return;
    }
    if (HANDLE worker = ::CreateThread(nullptr, 0, Driver, nullptr, 0, nullptr)) ::CloseHandle(worker);
    UE_LOGI("synctest: LOCAL ONLY role=%s peer=127.0.0.1 port=%u master=off signaling=off", host ? "host" : "client", config.port);
    ue_wrap::log::Flush();
    session_runtime::RunPlayLoop(host);
}
}

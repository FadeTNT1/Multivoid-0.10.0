// harness/autotest/autotest_saveui.cpp -- autonomous tests for host-only saving.
//
// The pair belongs together: both are the client-side half of host-only persistence, and both are
// client-only and verify from the log the module itself writes.
//   - RunAutonomousSaveBlockTest drives saveSlot.saveToSlot, so the verb gate's refusal is
//     observable without waiting for an autosave or a menu trigger.
//   - RunAutonomousSaveMessageTest drives the quicksave key's input event, so the refusal of a save
//     the player asked for, and its chat line, are observable without a real key press.

#include "harness/autotest.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

// Local copy of harness's ReadEnv (matches autotest.cpp's -- tiny, not worth crossing TUs).
std::string ReadEnv(const char* name) {
    char buf[256] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf) : std::string();
}

}  // namespace

// ---- the autonomous client save-block test ---------------------------------
//
// CLIENT-ONLY. Proves the verb gate (coop/save/save_block.cpp) CANCELS a world write on the
// client, not merely that it installed. Over a short run neither the autosave timer nor a menu save
// fires, so the write verb is driven by reflection: resolve the live saveSlot_C and call its
// saveToSlot. The client log shows `saveblock_test: invoking ...` followed by `save_block: refused
// the client's world write`, and no file of the client's changes. Gated by
// VOTVCOOP_RUN_SAVEBLOCK_TEST="1".
void RunAutonomousSaveBlockTest() {
    const bool isHost = !IsClientRole();
    if (isHost) {
        UE_LOGI("saveblock_test: not client -- this routine is client-only (the host's saves "
                "run). Returning.");
        return;
    }
    UE_LOGI("saveblock_test: starting on client (waiting 18 s: connect + possession + "
            "world load so saveSlot_C exists and the save_block gates are installed)");
    ::Sleep(18000);

    auto done = std::make_shared<std::atomic<int>>(0);  // 0=pending 1=invoked -1=resolve-fail
    GT::Post([done] {
        void* saveSlot = R::FindObjectByClass(L"saveSlot_C");
        if (!saveSlot) {
            UE_LOGW("saveblock_test: no live saveSlot_C instance -- world not fully "
                    "loaded? cannot exercise the save path");
            done->store(-1, std::memory_order_release);
            return;
        }
        void* fn = R::FindFunction(R::ClassOf(saveSlot), L"saveToSlot");
        if (!fn) {
            UE_LOGW("saveblock_test: saveToSlot UFunction not found on saveSlot_C -- "
                    "cannot exercise the save path");
            done->store(-1, std::memory_order_release);
            return;
        }
        // saveToSlot(asBackup, asAutosave): a zeroed frame is the main entry's write, which the
        // verb gate refuses at the body's entry on a client, so calling it directly writes
        // nothing.
        const int32_t frameSize = R::FunctionFrameSize(fn);
        std::vector<uint8_t> frame(frameSize > 0 ? static_cast<size_t>(frameSize) : 0, 0u);
        UE_LOGI("saveblock_test: invoking saveSlot.saveToSlot on client (saveSlot=%p, "
                "frameSize=%d) -- expect a 'save_block: refused' line next if the gate fired",
                saveSlot, frameSize);
        R::CallFunction(saveSlot, fn, frame.empty() ? nullptr : frame.data());
        done->store(1, std::memory_order_release);
    });

    for (int i = 0; i < 1000 && done->load(std::memory_order_acquire) == 0; ++i) ::Sleep(5);
    const int code = done->load(std::memory_order_acquire);
    if (code == 1) {
        UE_LOGI("saveblock_test: DONE -- saveToSlot invoked; grep this client log for "
                "'save_block: refused' to confirm the write was cancelled");
    } else {
        UE_LOGW("saveblock_test: did NOT invoke saveToSlot (resolve failed or task "
                "did not run) -- inconclusive");
    }
}

DWORD WINAPI SaveBlockTestThread(LPVOID /*arg*/) {
    RunAutonomousSaveBlockTest();
    return 0;
}

// ---- the autonomous client save-request test -------------------------------
//
// CLIENT-ONLY. Proves a save the client asks for is refused and answered: mainPlayer_C's quicksave
// input event is called by reflection, and its body calls mainGamemode.save from the player, as the
// key does. The client log shows `save_block: refused the client's save request` and the chat
// shows the not-the-host line. Gated by VOTVCOOP_RUN_SAVEMSG_TEST="1".
void RunAutonomousSaveMessageTest() {
    if (!IsClientRole()) {
        UE_LOGI("savemsg_test: not client -- this routine is client-only (the host's save runs). Returning.");
        return;
    }
    UE_LOGI("savemsg_test: starting on client (waiting 18 s: connect + possession + the save_block gates)");
    ::Sleep(18000);

    auto done = std::make_shared<std::atomic<int>>(0);  // 0=pending 1=drove -1=resolve-fail
    GT::Post([done] {
        void* mp = R::FindObjectByClass(P::name::MainPlayerClass);
        void* fn = mp ? R::FindFunction(R::ClassOf(mp), P::name::MainPlayerQuickSaveFn) : nullptr;
        if (!fn) {
            UE_LOGW("savemsg_test: mainPlayer_C=%p or its quicksave event not found -- cannot drive", mp);
            done->store(-1, std::memory_order_release);
            return;
        }
        const int32_t fs = R::FunctionFrameSize(fn);
        std::vector<uint8_t> frame(fs > 0 ? static_cast<size_t>(fs) : 0, 0u);
        UE_LOGI("savemsg_test: driving the quicksave event on client -- expect 'save_block: refused the "
                "client's save request' next");
        R::CallFunction(mp, fn, frame.empty() ? nullptr : frame.data());
        done->store(1, std::memory_order_release);
    });

    for (int i = 0; i < 1000 && done->load(std::memory_order_acquire) == 0; ++i) ::Sleep(5);
    UE_LOGI("savemsg_test: DONE (code=%d)", done->load(std::memory_order_acquire));
}

DWORD WINAPI SaveMessageTestThread(LPVOID /*arg*/) {
    RunAutonomousSaveMessageTest();
    return 0;
}

}  // namespace harness::autotest

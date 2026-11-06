#pragma once

namespace harness::autotest {
// Explicit local-only scenario; the normal menu and play paths never enter it.
void RunSyncTestScenario();
bool SyncTestNoInputCapture();
// Installs before the executable entry point; failure terminates this rig process.
void InstallSyncTestInputGuard();
}

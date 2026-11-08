// harness/dedicated_server_config.h -- the dedicated server's switch and its settings
// (docs/dedicated-server.md, "Configuration").
//
// The switch is `-multivoid-server` on the process command line or MULTIVOID_SERVER=1 in its
// environment; without it nothing of the server runs. The settings layer, lowest first: the
// defaults below, `[server]` of `server.ini` in the mod's data folder, the MULTIVOID_* environment,
// and `-mv<key>=<value>` on the command line. Win32 reads only (the command line, the environment,
// GetPrivateProfileString): no config row, no engine state, so it is safe on the boot thread before
// the first config read. Resolved once and latched; any thread after that.

#pragma once

#include <cstdint>
#include <string>

namespace harness::dedicated_server {

struct Settings {
    bool        enabled = false;
    uint16_t    port = 0;                       // 0: net.port keeps its own value
    std::string name = "Multivoid Server";      // the lobby name
    std::string password;                       // empty: an open session
    int         maxPlayers = 10;                // the server's own slot included, clamped 2..kMaxPeers (10)
    std::string saveSlot = "multivoid_server";  // the world: <slot>, s_<slot>, or a new story save
    bool        listPublic = true;              // announced to the master
    bool        brokered = false;               // false: DIRECT UDP listen on `port`
    std::string hostNick = "Server";            // the parked pawn's nickname
    int         autosaveMinutes = 10;           // 0: no autosave timer
    int         maxFps = 30;                    // t.MaxFPS once the world is up; 0 leaves it
    bool        stdoutMirror = true;            // every log line to stdout as well
    int         readyTimeoutS = 300;            // the host must be up within this, or the process exits
};

// Whether this process is a dedicated server. Latched on the first call. Any thread.
bool Enabled();

// The resolved settings (enabled == Enabled()). Latched on the first call. Any thread.
const Settings& Get();

// The settings as one log line, the password masked.
std::string Describe(const Settings& s);

}  // namespace harness::dedicated_server

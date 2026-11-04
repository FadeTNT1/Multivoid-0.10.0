// harness/dedicated_server_config.cpp -- see harness/dedicated_server_config.h.

#include "harness/dedicated_server_config.h"

#include "coop/player/players_registry.h"  // kMaxPeers
#include "ue_wrap/core/paths.h"

#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

namespace harness::dedicated_server {
namespace {

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                        nullptr, nullptr);
    std::string out(n > 0 ? static_cast<size_t>(n) : 0u, '\0');
    if (n > 0)
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                              nullptr);
    return out;
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// The process command line split the way the engine splits it (CommandLineToArgvW's rules).
const std::vector<std::wstring>& Args() {
    static std::vector<std::wstring> s_args;
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        int argc = 0;
        LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
        if (!argv) return;
        for (int i = 1; i < argc; ++i) s_args.emplace_back(argv[i]);
        ::LocalFree(argv);
    });
    return s_args;
}

bool HasArg(const wchar_t* flag) {
    const std::wstring want = Lower(flag);
    for (const std::wstring& a : Args())
        if (Lower(a) == want) return true;
    return false;
}

// `-mv<key>=<value>` (the key case-insensitive); a value may be quoted on the command line, which
// CommandLineToArgvW has already removed.
bool ArgValue(const wchar_t* key, std::string& out) {
    const std::wstring prefix = Lower(std::wstring(L"-mv") + key + L"=");
    for (const std::wstring& a : Args()) {
        if (a.size() >= prefix.size() && Lower(a.substr(0, prefix.size())) == prefix) {
            out = Narrow(a.substr(prefix.size()));
            return true;
        }
    }
    return false;
}

bool EnvValue(const char* name, std::string& out) {
    char buf[512] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return false;
    out.assign(buf, n);
    return true;
}

bool IniValue(const std::wstring& ini, const wchar_t* key, std::string& out) {
    if (ini.empty()) return false;
    // A sentinel no value can be, so a key present with an empty value still counts as set.
    constexpr const wchar_t* kAbsent = L"\x01absent\x01";
    wchar_t buf[512] = {};
    ::GetPrivateProfileStringW(L"server", key, kAbsent, buf, 512, ini.c_str());
    if (std::wstring(buf) == kAbsent) return false;
    out = Narrow(buf);
    return true;
}

// The value of one setting through the layers: the ini, then the environment, then the command
// line, each overriding the one before. False when no layer sets it.
bool Resolve(const std::wstring& ini, const wchar_t* iniKey, const char* envName,
             const wchar_t* argKey, std::string& out) {
    bool set = IniValue(ini, iniKey, out);
    if (envName) set = EnvValue(envName, out) || set;
    // The flag answers to its short key and to the ini key (-mvslot= and -mvsave_slot=), so a
    // name copied out of server.ini is never silently ignored.
    if (ArgValue(iniKey, out)) set = true;
    if (argKey) set = ArgValue(argKey, out) || set;
    return set;
}

bool ParseBool(const std::string& v, bool def) {
    std::string s = v;
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    return def;
}

int ParseInt(const std::string& v, int def, int lo, int hi) {
    if (v.empty()) return def;
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (!end || *end != '\0') return def;
    return static_cast<int>(std::clamp<long>(n, lo, hi));
}

Settings Build() {
    Settings s;
    std::string v;
    s.enabled = HasArg(L"-multivoid-server") || (EnvValue("MULTIVOID_SERVER", v) && ParseBool(v, false));
    if (!s.enabled) return s;
    const std::wstring dir = ue_wrap::paths::ModDataDir();
    const std::wstring ini = dir.empty() ? std::wstring() : dir + L"\\server.ini";

    if (Resolve(ini, L"port", "MULTIVOID_PORT", L"port", v))
        s.port = static_cast<uint16_t>(ParseInt(v, 0, 0, 65535));
    if (Resolve(ini, L"name", "MULTIVOID_SERVER_NAME", L"name", v) && !v.empty()) s.name = v;
    if (Resolve(ini, L"password", "MULTIVOID_PASSWORD", L"password", v)) s.password = v;
    if (Resolve(ini, L"max_players", "MULTIVOID_MAX_PLAYERS", L"maxplayers", v))
        s.maxPlayers = ParseInt(v, s.maxPlayers, 2, static_cast<int>(coop::players::kMaxPeers));
    s.maxPlayers = std::clamp(s.maxPlayers, 2, static_cast<int>(coop::players::kMaxPeers));
    if (Resolve(ini, L"save_slot", "MULTIVOID_SAVE_SLOT", L"slot", v) && !v.empty()) s.saveSlot = v;
    if (Resolve(ini, L"list_public", "MULTIVOID_LIST_PUBLIC", L"list", v))
        s.listPublic = ParseBool(v, s.listPublic);
    if (Resolve(ini, L"reach", "MULTIVOID_REACH", L"reach", v)) {
        std::string r = v;
        for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        s.brokered = (r == "brokered" || r == "p2p");
    }
    if (Resolve(ini, L"host_nick", "MULTIVOID_HOST_NICK", L"nick", v) && !v.empty()) s.hostNick = v;
    if (Resolve(ini, L"autosave_minutes", "MULTIVOID_AUTOSAVE_MINUTES", L"autosave", v))
        s.autosaveMinutes = ParseInt(v, s.autosaveMinutes, 0, 24 * 60);
    if (Resolve(ini, L"max_fps", "MULTIVOID_MAX_FPS", L"maxfps", v))
        s.maxFps = ParseInt(v, s.maxFps, 0, 1000);
    if (Resolve(ini, L"stdout", "MULTIVOID_STDOUT", L"stdout", v))
        s.stdoutMirror = ParseBool(v, s.stdoutMirror);
    if (Resolve(ini, L"ready_timeout_s", "MULTIVOID_READY_TIMEOUT", nullptr, v))
        s.readyTimeoutS = ParseInt(v, s.readyTimeoutS, 30, 3600);
    return s;
}

}  // namespace

const Settings& Get() {
    static const Settings s_settings = Build();
    return s_settings;
}

bool Enabled() { return Get().enabled; }

std::string Describe(const Settings& s) {
    std::string out = "name='" + s.name + "' slot='" + s.saveSlot + "' port=" +
                      (s.port ? std::to_string(s.port) : std::string("(net.port)")) +
                      " reach=" + (s.brokered ? "brokered" : "direct") +
                      " listed=" + (s.listPublic || s.brokered ? "1" : "0") +
                      " max_players=" + std::to_string(s.maxPlayers) +
                      " password=" + (s.password.empty() ? "(none)" : "(set)") +
                      " nick='" + s.hostNick + "' autosave_min=" + std::to_string(s.autosaveMinutes) +
                      " max_fps=" + std::to_string(s.maxFps) + " stdout=" + (s.stdoutMirror ? "1" : "0");
    return out;
}

}  // namespace harness::dedicated_server

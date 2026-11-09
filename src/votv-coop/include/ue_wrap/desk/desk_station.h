// ue_wrap/desk/desk_station.h -- which station of the main desk (analogDScreenTest_C) a component belongs to, by the
// component's name. Pure mapping, no engine access, so a host-compiled test can run it
// (.github/ci/desk_station_test.cpp). The names are version-specific and live in ue_wrap/core/sdk_profile_names.h.
//
// The desk is one actor with several banks of physical buttons. Only the coordinate cluster is an enterable
// screen (its button runs `enter interface` on screen_coords); every other bank acts where it is pressed. The
// occupancy gate denies a second peer the coordinate cluster alone (coop/interactables/device_occupancy).

#pragma once

#include "ue_wrap/core/sdk_profile_names.h"

#include <cstdint>
#include <string_view>

namespace ue_wrap::desk_station {

enum class Station : uint8_t {
    Other,        // a desk component that is no station's control: the stand, a back panel, a module collider
    Coordinates,  // button_coords, screen_coords, screenbutton_switchtocoord*, screenbutton_bringcoord*: the screen
    Download,     // button_downl_*: tuning, filters, polarity, save, delete, the detector
    Playback,     // button_play_*: the deck
    Refiner,      // button_comp_*: upload, start, stop, max level
    Phone,        // button_phone
};

// Only the coordinate screen is claimed: it has an enter and an exit the game makes, the buttons have neither.
constexpr bool IsClaimed(Station s) { return s == Station::Coordinates; }

namespace detail {

constexpr wchar_t Lower(wchar_t c) { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c; }

// UE names compare without case; the capture the constants come from is lower-cased.
constexpr bool SameName(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (Lower(a[i]) != Lower(b[i])) return false;
    return true;
}

constexpr bool HasPrefix(std::wstring_view name, std::wstring_view prefix) {
    return name.size() >= prefix.size() && SameName(name.substr(0, prefix.size()), prefix);
}

}  // namespace detail

// The station a component named `name` belongs to; Other for any name that is none's (including an empty one).
constexpr Station ClassifyName(std::wstring_view name) {
    namespace N = ue_wrap::profile::name;
    using detail::HasPrefix;
    using detail::SameName;
    if (SameName(name, N::DeskCoordButton) || SameName(name, N::DeskCoordScreen) ||
        HasPrefix(name, N::DeskCoordSwitchPrefix) || HasPrefix(name, N::DeskCoordBringPrefix))
        return Station::Coordinates;
    if (HasPrefix(name, N::DeskDownloadButtonPrefix) || SameName(name, N::DeskDownloadScreen)) return Station::Download;
    if (HasPrefix(name, N::DeskPlaybackButtonPrefix) || SameName(name, N::DeskPlaybackScreen)) return Station::Playback;
    if (HasPrefix(name, N::DeskRefinerButtonPrefix) || SameName(name, N::DeskRefinerScreen)) return Station::Refiner;
    if (SameName(name, N::DeskPhoneButton)) return Station::Phone;
    return Station::Other;
}

// The station in plain words, for a log line or a notice.
constexpr const wchar_t* StationName(Station s) {
    switch (s) {
    case Station::Coordinates: return L"coordinate screen";
    case Station::Download:    return L"download controls";
    case Station::Playback:    return L"playback controls";
    case Station::Refiner:     return L"refiner controls";
    case Station::Phone:       return L"phone";
    default:                   return L"desk";
    }
}

}  // namespace ue_wrap::desk_station

// The main desk's component-to-station mapping (ue_wrap/desk/desk_station.h) against the component names the
// 0.10.0 bytecode capture lists. Only the coordinate cluster is claimed by the occupancy gate; every other
// component must pass it. The header is pure, so this compiles the production file itself.
#include "ue_wrap/desk/desk_station.h"

#include <cstdio>

namespace DS = ue_wrap::desk_station;
using DS::Station;

static int g_fail = 0;

static void Expect(const wchar_t* name, Station want) {
    const Station got = DS::ClassifyName(name);
    if (got != want) {
        std::printf("FAIL %ls: got %d, wanted %d\n", name, static_cast<int>(got), static_cast<int>(want));
        ++g_fail;
    }
}

int main() {
    // The coordinate screen: its enter button, its screen, its cursor buttons.
    for (const wchar_t* n : {L"button_coords", L"screen_coords", L"screenbutton_switchtocoord1",
                             L"screenbutton_switchtocoord2", L"screenbutton_switchtocoord3",
                             L"screenbutton_bringcoord1", L"screenbutton_bringcoord2", L"screenbutton_bringcoord3",
                             L"screenbutton_bringcoordcent", L"Button_Coords", L"SCREEN_COORDS"})
        Expect(n, Station::Coordinates);

    for (const wchar_t* n : {L"button_downl_delsig", L"button_downl_ff_spdadd_1", L"button_downl_ff_spdadd_15",
                             L"button_downl_ff_spdadd_5", L"button_downl_ff_toggle", L"button_downl_pdir_toggle",
                             L"button_downl_pf_spdadd_1", L"button_downl_pf_spdadd_15", L"button_downl_pf_spdadd_5",
                             L"button_downl_pf_toggle", L"button_downl_savesig1", L"screen_download"})
        Expect(n, Station::Download);

    for (const wchar_t* n : {L"button_play_left", L"button_play_remapmax", L"button_play_remapmin",
                             L"button_play_right", L"button_play_savesig", L"button_play_scroll", L"button_play_vis",
                             L"button_play_vol", L"screen_play"})
        Expect(n, Station::Playback);

    for (const wchar_t* n : {L"button_comp_maxlevel", L"button_comp_start", L"button_comp_stop",
                             L"button_comp_upload", L"screen_comp"})
        Expect(n, Station::Refiner);

    Expect(L"button_phone", Station::Phone);

    // No station's control: unclaimed, so the gate lets a press on it through.
    for (const wchar_t* n : {L"", L"stand", L"mod1", L"StaticMeshComponent0", L"button_coordsX", L"button_",
                             L"screenbutton_other", L"screen_coord", L"button_down", L"button_playback"})
        Expect(n, Station::Other);

    // Which stations the gate holds a second peer out of: the coordinate screen alone.
    for (Station s : {Station::Other, Station::Download, Station::Playback, Station::Refiner, Station::Phone}) {
        if (DS::IsClaimed(s)) { std::printf("FAIL station %d is claimed\n", static_cast<int>(s)); ++g_fail; }
    }
    if (!DS::IsClaimed(Station::Coordinates)) { std::printf("FAIL the coordinate screen is not claimed\n"); ++g_fail; }

    static_assert(DS::ClassifyName(L"button_coords") == Station::Coordinates, "constexpr");
    static_assert(DS::ClassifyName(L"button_play_vol") == Station::Playback, "constexpr");

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("desk_station: ok\n");
    return 0;
}

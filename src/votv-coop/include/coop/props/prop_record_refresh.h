// coop/props/prop_record_refresh.h -- a prop whose save record changes while it sits in place
// republishes that record. Two classes first: the drive box (Aprop_box_C, the "data case") and the
// tape reel case (Aprop_reelbox_C). Overview: docs/props.md.
//
// prop_save_data carries a prop's record on every BIRTH path -- a spawn, a drop intent, the join
// snapshot -- and on none other, so a prop whose save state changes while it sits there never
// tells anyone. Both classes keep a lid and their contents in the record: the box `opened` and
// `drives_in`, the reel case `lid`, `reeltop` and `reelBottom` (a reel's progress, -1 = empty).
// Taking a lid off spawns it in the presser's hands; putting a drive or reel in destroys the held
// one and stores it in the record. So a guest's open left every other peer with a closed case,
// and a reel a guest put in vanished for the host: its destroy crossed, the record did not.
//
// The same lane carries the lamps (floor, desk, wall light, spotlight head), the table clock, the
// second TV's settings (updproperties) and the barbecue's lid and coals; the row list and the
// classes left out (mini oven, cooking pad, wood chipper) are in the .cpp.
//
// The verb watched is each class's own upd(): every mutation ends there and no tick or look-at
// path calls it. The watch is a script_gate one, not a ProcessEvent observer -- the prop calls its
// own upd() from inside its Blueprint, a route ProcessEvent never sees. The .cpp carries the rest.

#pragma once

namespace coop::net { class Session; }

namespace coop::props::prop_record_refresh {

// Watch each class's refresh verb and cache the session. Idempotent; a class not loaded yet is
// retried on the next call. Game thread.
void Install(coop::net::Session* session);

// Session teardown: release the session and reset publication counters.
void OnDisconnect();

}  // namespace coop::props::prop_record_refresh

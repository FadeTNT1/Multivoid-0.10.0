// ue_wrap/devices/water_container.h -- engine access for the bucket (prop_bucket_C), the prop a tap fills. Principle-7
// engine-wrapper layer: reflection, offsets and the repaint verb, no network or coop state; coop::water_fill_sync owns
// those and reaches the engine through here.
//
// A bucket's water is `height` (what a tap's tick over it writes, and its own tick lowers by the tilt), with
// `height_interp` the level its look follows (its tick eases it toward `height`; its loadData sets both) and `empty`
// the flag the tap's tick clears and its own tick recomputes. Its no-arg upd() repaints from them. The names are in
// ue_wrap/core/sdk_profile_names.h; offsets resolve by name and type, never by number.

#pragma once

namespace ue_wrap::water_container {

// Resolve the bucket class and its members as it streams in. Idempotent; true once resolved. Game thread.
bool EnsureResolved();

// True iff `obj`'s class is prop_bucket_C or below, once resolved. Class-pure.
bool IsBucket(void* obj);

struct Bucket {
    float height = 0.f;
    float interp = 0.f;
    bool  empty = false;
};

// Read / write a bucket's water. The caller has established the object IS a bucket (IsBucket, or the scan hub's
// class-pure verdict): these check only that the members resolved, because the tap's tick reads them every frame.
// False when they did not (`out` is left alone). WriteBucket writes `height` and `empty`, and `height_interp` too when
// `snapInterp`; no verb runs.
bool ReadBucket(void* bucket, Bucket& out);
bool WriteBucket(void* bucket, float height, bool empty, bool snapInterp);

// The bucket's own upd(), which repaints from the fields. False when it did not run.
bool RepaintBucket(void* bucket);

// Whether the bucket's body simulates physics here (its tick, and so the easing of its look, only runs then).
bool BucketSimulating(void* bucket);

}  // namespace ue_wrap::water_container

// ue_wrap/core/script_stack.cpp -- see ue_wrap/core/script_stack.h.

#include "ue_wrap/core/script_stack.h"

#include "ue_wrap/core/sdk_profile.h"

#include <windows.h>

#include <cstdint>

namespace ue_wrap::script_stack {
namespace {

namespace P = ue_wrap::profile;

// The open dispatches, outermost at index 0. Plain pointer arrays, zero-initialised, so the thread-local
// storage needs no constructor on a thread the engine created.
thread_local void*    t_object[kMaxDispatches];
thread_local void*    t_function[kMaxDispatches];
thread_local int      t_depth  = 0;
thread_local uint64_t t_serial = 0;

// A stack overflow is passed on, as the detour's own firewalls pass it.
int ReadFault(unsigned long code) {
    return code == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER;
}

// One frame's three fields; 0 when a read faulted. Raw out-pointers only: no C++ object in a __try frame.
int ReadFrameSEH(const void* frame, void** object, void** function, const void** previous) {
    __try {
        const auto* f = static_cast<const uint8_t*>(frame);
        *object   = *reinterpret_cast<void* const*>(f + P::off::FFrame_Object);
        *function = *reinterpret_cast<void* const*>(f + P::off::FFrame_Node);
        *previous = *reinterpret_cast<const void* const*>(f + P::off::FFrame_PreviousFrame);
        return 1;
    } __except (ReadFault(GetExceptionCode())) {
        return 0;
    }
}

}  // namespace

int WalkFrames(const void* frame, Entry* out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = 0;
    for (int i = 0; frame && i < kMaxFrames && n < cap; ++i) {
        void* object = nullptr;
        void* function = nullptr;
        const void* previous = nullptr;
        if (!ReadFrameSEH(frame, &object, &function, &previous)) break;
        out[n].object = object;
        out[n].function = function;
        ++n;
        if (previous == frame) break;
        frame = previous;
    }
    return n;
}

int OpenDispatches(Entry* out, int cap) {
    if (!out || cap <= 0) return 0;
    const int kept = t_depth < kMaxDispatches ? t_depth : kMaxDispatches;
    int n = 0;
    for (int i = kept - 1; i >= 0 && n < cap; --i) {
        out[n].object = t_object[i];
        out[n].function = t_function[i];
        ++n;
    }
    return n;
}

Entry OutermostDispatch() {
    if (t_depth <= 0) return Entry{nullptr, nullptr};
    return Entry{t_object[0], t_function[0]};
}

uint64_t OutermostDispatchSerial() { return t_depth > 0 ? t_serial : 0; }

namespace detail {

void PushDispatch(void* object, void* function) {
    if (t_depth == 0) ++t_serial;
    if (t_depth >= 0 && t_depth < kMaxDispatches) {
        t_object[t_depth] = object;
        t_function[t_depth] = function;
    }
    ++t_depth;
}

void PopDispatch() {
    if (t_depth > 0) --t_depth;
}

}  // namespace detail

}  // namespace ue_wrap::script_stack

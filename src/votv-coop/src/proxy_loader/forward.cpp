// proxy_loader/forward.cpp -- the lazy half of the dwmapi export forwarding.
//
// Every export of the real dwmapi.dll is a stub (dwmapi_stubs.asm, generated) that tail-jumps
// through mv_targets[slot]. A slot starts at the asm resolver, which calls mv_resolve_slot below on
// the first call: it maps the REAL dwmapi.dll from System32 by its full path (the bare name "dwmapi.dll"
// would match this very module and recurse), looks the export up by name -- or by ordinal for the
// ordinal-only ones -- patches the slot, and the call carries on to the real function. Nothing here runs
// under the loader lock: the first call is made by the game, long after DllMain returned.

#include <windows.h>

#include "dwmapi_exports.inc"

extern "C" {
// Defined in dwmapi_stubs.asm.
extern void* mv_targets[kSlotCount];

// Stands in for an export the running Windows' dwmapi does not have (a newer export than the OS knows, or an
// undocumented ordinal that moved). Nearly the whole dwmapi surface returns HRESULT, so "not implemented".
static HRESULT __stdcall MissingExport() { return E_NOTIMPL; }

// Called by mv_resolve (asm) with the slot in ECX; returns the address to jump to. Never fails.
void* mv_resolve_slot(unsigned slot) {
    static HMODULE real = nullptr;
    if (!real) {
        wchar_t path[MAX_PATH + 16] = {};
        const UINT n = ::GetSystemDirectoryW(path, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            ::lstrcatW(path, L"\\dwmapi.dll");
            real = ::LoadLibraryExW(path, nullptr, 0);
        }
    }
    void* target = nullptr;
    if (real && slot < kSlotCount) {
        const char* name = kExportNames[slot];
        target = reinterpret_cast<void*>(::GetProcAddress(
            real, name ? name : MAKEINTRESOURCEA(kOrdinalBase + slot)));
    }
    if (!target) target = reinterpret_cast<void*>(&MissingExport);
    if (slot < kSlotCount) mv_targets[slot] = target;  // a racing first call stores the same value
    return target;
}
}  // extern "C"

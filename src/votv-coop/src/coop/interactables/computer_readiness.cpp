// coop/interactables/computer_readiness.cpp -- see coop/interactables/computer_readiness.h.

#include "coop/interactables/computer_readiness.h"

#include "coop/config/config.h"

#include "ue_wrap/core/log.h"

#include <cstdarg>
#include <cstdio>

namespace coop::computer_readiness {
namespace {

constexpr size_t kSites = static_cast<size_t>(Site::Count);
constexpr size_t kWhys = static_cast<size_t>(Why::Count);

const char* const kSiteText[kSites] = {"sat line",    "sat merge",  "sat input",
                                       "sat publish", "desk press", "laptop power"};
const char* const kWhyText[kWhys] = {"missing actor",         "missing widget",        "dispatch failed",
                                     "native no-op",          "waiting on a latent continuation",
                                     "output merged",         "state changed"};

bool g_said[kSites][kWhys] = {};

bool Failing(Why why) {
    return why == Why::MissingActor || why == Why::MissingWidget || why == Why::DispatchFailed ||
           why == Why::NativeNoOp;
}

}  // namespace

bool TraceOn() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::computer_readiness_trace);
    return s;
}

void Report(Site site, Why why, const char* fmt, ...) {
    const size_t si = static_cast<size_t>(site);
    const size_t wi = static_cast<size_t>(why);
    if (si >= kSites || wi >= kWhys) return;
    const bool first = Failing(why) && !g_said[si][wi];
    if (!first && !TraceOn()) return;
    char detail[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    detail[sizeof(detail) - 1] = 0;
    if (first) {
        g_said[si][wi] = true;
        UE_LOGW("computer: %s -- %s: %s (first of its kind; computer_readiness_trace=1 logs each)", kSiteText[si],
                kWhyText[wi], detail);
    } else {
        UE_LOGI("computer: %s -- %s: %s", kSiteText[si], kWhyText[wi], detail);
    }
}

void ResetSite(Site site) {
    const size_t si = static_cast<size_t>(site);
    if (si >= kSites) return;
    for (bool& b : g_said[si]) b = false;
}

}  // namespace coop::computer_readiness
